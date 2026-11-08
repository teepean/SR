/**
 *
 *  The window system on Godot (host.h backend): LibGodot owns the game window and is stepped by the game -
 *  one Godot frame (GodotInstance::iteration) per presented game frame, and a few per second while the game
 *  only processes messages (loading).
 *
 *  libgodot is loaded with dlopen(RTLD_LOCAL | RTLD_DEEPBIND): it exports its bundled third-party libraries
 *  (FreeType, zlib, ...), which must neither replace the system ones the runtime uses nor be replaced by them.
 *  (RTLD_DEEPBIND is glibc's; macOS keeps each library's symbols apart anyway: two-level namespaces.)
 *  godot-cpp (linked in) talks to it only through the GDExtension interface it hands to host_init.
 *
 *  Picture: the game's framebuffer (XRGB8888, CPU-drawn 2D) is a texture on a canvas item covering the window;
 *  a shader swaps the channels (the bytes are B, G, R, X). The 3D renderer will draw below it.
 *  Input: a node in the scene tree turns Godot's input events into SDL_Event structures (what user32 and
 *  dinput read) in a queue of its own; key and button states are kept here.
 *
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dlfcn.h>
#include <pthread.h>
#include <limits.h>
#include <vector>
#include <string>
#include <algorithm>

#include <godot_cpp/godot.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/classes/godot_instance.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/classes/window.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/display_server.hpp>
#include <godot_cpp/classes/input.hpp>
#include <godot_cpp/classes/input_event.hpp>
#include <godot_cpp/classes/input_event_key.hpp>
#include <godot_cpp/classes/input_event_mouse_button.hpp>
#include <godot_cpp/classes/input_event_mouse_motion.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/viewport_texture.hpp>
#include <godot_cpp/classes/canvas_layer.hpp>
#include <godot_cpp/classes/label.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>

#include <SDL.h>

#include "config.h"

using namespace godot;

#define eprintf(...) fprintf(stderr, __VA_ARGS__)

extern "C" int winapi_debug;

/* ------------------------------------------------------------------ */
/* state                                                               */

typedef GDExtensionObjectPtr (*create_instance_fn)(int, char **, GDExtensionInitializationFunction);
typedef void (*destroy_instance_fn)(GDExtensionObjectPtr);

static void *libgodot;
static create_instance_fn create_instance;
static destroy_instance_fn destroy_instance;
static GDExtensionObjectPtr instance_ptr;
static GodotInstance *instance;
static int started;
static int quitting;

// Godot objects: created after Godot's start (godot-cpp's types call into the engine from their constructors,
// so none of them can be static)
struct godot_state
{
    RID canvas, canvas_item, texture, shader, material;
    Ref<Image> frame_image;
    PackedByteArray frame_bytes;
};
static godot_state *gs;

// Godot is driven from the thread that created it (the game's main thread); the engine also draws from
// others (AvP2's loading screen runs in its own thread): their frames wait here for the main thread
static pthread_t main_thread;
static pthread_mutex_t pending_mutex = PTHREAD_MUTEX_INITIALIZER;
static std::vector<uint32_t> pending_pixels;
static int pending_w, pending_h, pending;

static int on_main_thread(void)
{
    return pthread_equal(pthread_self(), main_thread);
}

// for render3d.cpp: Godot is up and this is the thread that may call it
extern "C" int ghost_ready(void)
{
    return started && !quitting && on_main_thread();
}

// window state the game asks for from any thread; applied on the main thread before each Godot frame
static struct
{
    int size_w, size_h, size;
    std::string title;
    int set_title;
    int mouse_mode;
    void *cursor;
    int set_cursor;
    int toggle_fullscreen;
    int warp_x, warp_y, warp;
} want;

static void apply_wanted(void);
static int req_w, req_h;            // the window size last asked for
static uint32_t req_time;
static int tex_w, tex_h;
static uint32_t last_iteration;

static SDL_Event queue[512];
static int queue_head, queue_count;
static uint8_t keys[SDL_NUM_SCANCODES];
static uint32_t mouse_buttons;
static int mouse_x, mouse_y;
static int relative_mouse, cursor_hidden;

static void push_event(const SDL_Event &ev)
{
    if (queue_count == (int)(sizeof(queue) / sizeof(queue[0])))
    {
        // full: drop the oldest
        queue_head = (queue_head + 1) % (int)(sizeof(queue) / sizeof(queue[0]));
        queue_count--;
    }
    queue[(queue_head + queue_count) % (int)(sizeof(queue) / sizeof(queue[0]))] = ev;
    queue_count++;
}

/* ------------------------------------------------------------------ */
/* keys: Godot's physical keycodes (US layout positions) -> SDL scancodes */

static SDL_Scancode scancode_of(Key k, KeyLocation loc)
{
    if ((k >= KEY_A) && (k <= KEY_Z)) return (SDL_Scancode)(SDL_SCANCODE_A + (k - KEY_A));
    if ((k >= KEY_1) && (k <= KEY_9)) return (SDL_Scancode)(SDL_SCANCODE_1 + (k - KEY_1));
    if ((k >= KEY_F1) && (k <= KEY_F12)) return (SDL_Scancode)(SDL_SCANCODE_F1 + (k - KEY_F1));
    if ((k >= KEY_KP_1) && (k <= KEY_KP_9)) return (SDL_Scancode)(SDL_SCANCODE_KP_1 + (k - KEY_KP_1));
    const int right = (loc == KEY_LOCATION_RIGHT);
    switch (k)
    {
        case KEY_0: return SDL_SCANCODE_0;
        case KEY_ESCAPE: return SDL_SCANCODE_ESCAPE;
        case KEY_TAB: return SDL_SCANCODE_TAB;
        case KEY_BACKSPACE: return SDL_SCANCODE_BACKSPACE;
        case KEY_ENTER: return SDL_SCANCODE_RETURN;
        case KEY_KP_ENTER: return SDL_SCANCODE_KP_ENTER;
        case KEY_INSERT: return SDL_SCANCODE_INSERT;
        case KEY_DELETE: return SDL_SCANCODE_DELETE;
        case KEY_PAUSE: return SDL_SCANCODE_PAUSE;
        case KEY_PRINT: return SDL_SCANCODE_PRINTSCREEN;
        case KEY_HOME: return SDL_SCANCODE_HOME;
        case KEY_END: return SDL_SCANCODE_END;
        case KEY_LEFT: return SDL_SCANCODE_LEFT;
        case KEY_UP: return SDL_SCANCODE_UP;
        case KEY_RIGHT: return SDL_SCANCODE_RIGHT;
        case KEY_DOWN: return SDL_SCANCODE_DOWN;
        case KEY_PAGEUP: return SDL_SCANCODE_PAGEUP;
        case KEY_PAGEDOWN: return SDL_SCANCODE_PAGEDOWN;
        case KEY_SHIFT: return right ? SDL_SCANCODE_RSHIFT : SDL_SCANCODE_LSHIFT;
        case KEY_CTRL: return right ? SDL_SCANCODE_RCTRL : SDL_SCANCODE_LCTRL;
        case KEY_ALT: return right ? SDL_SCANCODE_RALT : SDL_SCANCODE_LALT;
        case KEY_META: return right ? SDL_SCANCODE_RGUI : SDL_SCANCODE_LGUI;
        case KEY_CAPSLOCK: return SDL_SCANCODE_CAPSLOCK;
        case KEY_NUMLOCK: return SDL_SCANCODE_NUMLOCKCLEAR;
        case KEY_SCROLLLOCK: return SDL_SCANCODE_SCROLLLOCK;
        case KEY_KP_MULTIPLY: return SDL_SCANCODE_KP_MULTIPLY;
        case KEY_KP_DIVIDE: return SDL_SCANCODE_KP_DIVIDE;
        case KEY_KP_SUBTRACT: return SDL_SCANCODE_KP_MINUS;
        case KEY_KP_ADD: return SDL_SCANCODE_KP_PLUS;
        case KEY_KP_PERIOD: return SDL_SCANCODE_KP_PERIOD;
        case KEY_KP_0: return SDL_SCANCODE_KP_0;
        case KEY_MENU: return SDL_SCANCODE_APPLICATION;
        case KEY_SPACE: return SDL_SCANCODE_SPACE;
        case KEY_APOSTROPHE: return SDL_SCANCODE_APOSTROPHE;
        case KEY_COMMA: return SDL_SCANCODE_COMMA;
        case KEY_MINUS: return SDL_SCANCODE_MINUS;
        case KEY_PERIOD: return SDL_SCANCODE_PERIOD;
        case KEY_SLASH: return SDL_SCANCODE_SLASH;
        case KEY_SEMICOLON: return SDL_SCANCODE_SEMICOLON;
        case KEY_EQUAL: return SDL_SCANCODE_EQUALS;
        case KEY_BRACKETLEFT: return SDL_SCANCODE_LEFTBRACKET;
        case KEY_BACKSLASH: return SDL_SCANCODE_BACKSLASH;
        case KEY_BRACKETRIGHT: return SDL_SCANCODE_RIGHTBRACKET;
        case KEY_QUOTELEFT: return SDL_SCANCODE_GRAVE;
        case KEY_SECTION: return SDL_SCANCODE_NONUSBACKSLASH;
        default: return SDL_SCANCODE_UNKNOWN;
    }
}

static uint16_t current_mods(void)
{
    uint16_t m = 0;
    if (keys[SDL_SCANCODE_LSHIFT]) m |= KMOD_LSHIFT;
    if (keys[SDL_SCANCODE_RSHIFT]) m |= KMOD_RSHIFT;
    if (keys[SDL_SCANCODE_LCTRL]) m |= KMOD_LCTRL;
    if (keys[SDL_SCANCODE_RCTRL]) m |= KMOD_RCTRL;
    if (keys[SDL_SCANCODE_LALT]) m |= KMOD_LALT;
    if (keys[SDL_SCANCODE_RALT]) m |= KMOD_RALT;
    return m;
}

/* ------------------------------------------------------------------ */
/* the input node                                                      */

class AvP2Host : public Node
{
    GDCLASS(AvP2Host, Node);

protected:
    static void _bind_methods() {}

public:
    void _input(const Ref<InputEvent> &p_event) override
    {
        SDL_Event ev;
        memset(&ev, 0, sizeof(ev));

        Ref<InputEventKey> k = p_event;
        if (k.is_valid())
        {
            SDL_Scancode sc = scancode_of(k->get_physical_keycode(), k->get_location());
            if (sc == SDL_SCANCODE_UNKNOWN) return;
            keys[sc] = k->is_pressed() ? 1 : 0;
            ev.type = k->is_pressed() ? SDL_KEYDOWN : SDL_KEYUP;
            ev.key.state = k->is_pressed() ? SDL_PRESSED : SDL_RELEASED;
            ev.key.repeat = k->is_echo() ? 1 : 0;
            ev.key.keysym.scancode = sc;
            ev.key.keysym.mod = current_mods();
            push_event(ev);
            return;
        }

        Ref<InputEventMouseMotion> mm = p_event;
        if (mm.is_valid())
        {
            Vector2 pos = mm->get_position(), rel = mm->get_screen_relative();
            mouse_x = (int)pos.x;
            mouse_y = (int)pos.y;
            ev.type = SDL_MOUSEMOTION;
            ev.motion.x = mouse_x;
            ev.motion.y = mouse_y;
            ev.motion.xrel = (int)rel.x;
            ev.motion.yrel = (int)rel.y;
            ev.motion.state = mouse_buttons;
            push_event(ev);
            return;
        }

        Ref<InputEventMouseButton> mb = p_event;
        if (mb.is_valid())
        {
            MouseButton b = mb->get_button_index();
            Vector2 pos = mb->get_position();
            if ((b == MOUSE_BUTTON_WHEEL_UP) || (b == MOUSE_BUTTON_WHEEL_DOWN))
            {
                if (!mb->is_pressed()) return;
                ev.type = SDL_MOUSEWHEEL;
                ev.wheel.y = (b == MOUSE_BUTTON_WHEEL_UP) ? 1 : -1;
                push_event(ev);
                return;
            }
            uint8_t sb;
            switch (b)
            {
                case MOUSE_BUTTON_LEFT: sb = SDL_BUTTON_LEFT; break;
                case MOUSE_BUTTON_RIGHT: sb = SDL_BUTTON_RIGHT; break;
                case MOUSE_BUTTON_MIDDLE: sb = SDL_BUTTON_MIDDLE; break;
                case MOUSE_BUTTON_XBUTTON1: sb = SDL_BUTTON_X1; break;
                case MOUSE_BUTTON_XBUTTON2: sb = SDL_BUTTON_X2; break;
                default: return;
            }
            if (mb->is_pressed()) mouse_buttons |= SDL_BUTTON(sb); else mouse_buttons &= ~SDL_BUTTON(sb);
            mouse_x = (int)pos.x;
            mouse_y = (int)pos.y;
            ev.type = mb->is_pressed() ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP;
            ev.button.button = sb;
            ev.button.state = mb->is_pressed() ? SDL_PRESSED : SDL_RELEASED;
            ev.button.x = mouse_x;
            ev.button.y = mouse_y;
            push_event(ev);
            return;
        }
    }

    void _notification(int p_what)
    {
        SDL_Event ev;
        memset(&ev, 0, sizeof(ev));
        switch (p_what)
        {
            case NOTIFICATION_WM_CLOSE_REQUEST:
                ev.type = SDL_QUIT;
                push_event(ev);
                break;
            case NOTIFICATION_APPLICATION_FOCUS_IN:
            case NOTIFICATION_APPLICATION_FOCUS_OUT:
                // tests: AVP2_KEEP_ACTIVE=1 keeps the game active without the focus (it pauses and stops drawing
                // otherwise, which spoils automated runs when something else on the desktop takes the focus)
                if (getenv("AVP2_KEEP_ACTIVE") && atoi(getenv("AVP2_KEEP_ACTIVE"))) break;
                ev.type = SDL_WINDOWEVENT;
                ev.window.event = (p_what == NOTIFICATION_APPLICATION_FOCUS_IN) ? SDL_WINDOWEVENT_FOCUS_GAINED : SDL_WINDOWEVENT_FOCUS_LOST;
                if (p_what == NOTIFICATION_APPLICATION_FOCUS_OUT)
                {
                    memset(keys, 0, sizeof(keys));
                    mouse_buttons = 0;
                }
                push_event(ev);
                break;
        }
    }
};

static void init_module(ModuleInitializationLevel level)
{
    if (level == MODULE_INITIALIZATION_LEVEL_SCENE) ClassDB::register_class<AvP2Host>();
}

static void term_module(ModuleInitializationLevel) {}

extern "C" GDExtensionBool avp2_godot_init(GDExtensionInterfaceGetProcAddress gpa, GDExtensionClassLibraryPtr lib, GDExtensionInitialization *init)
{
    GDExtensionBinding::InitObject obj(gpa, lib, init);
    obj.register_initializer(init_module);
    obj.register_terminator(term_module);
    obj.set_minimum_library_initialization_level(MODULE_INITIALIZATION_LEVEL_SCENE);
    return obj.init();
}

/* ------------------------------------------------------------------ */
/* loading and starting                                                */

static std::string exe_dir(void)
{
    char *base = SDL_GetBasePath();     // the program's directory, with a separator at the end (any platform)
    if (base == NULL) return ".";
    std::string d = base;
    SDL_free(base);
    if ((d.size() > 1) && ((d.back() == '/') || (d.back() == '\\'))) d.pop_back();
    return d;
}

#if defined(__APPLE__)
#define LIBGODOT_NAME "libgodot.dylib"
#elif defined(_WIN32)
#define LIBGODOT_NAME "libgodot.dll"
#else
#define LIBGODOT_NAME "libgodot.so"
#endif
#ifndef RTLD_DEEPBIND
#define RTLD_DEEPBIND 0
#endif

static std::string find_file(const char *env, const char *cfg, const std::vector<std::string> &candidates)
{
    const char *e = getenv(env);
    if (e != NULL) return e;
    e = config_get(cfg);
    if (e != NULL) return e;
    for (const std::string &c : candidates)
    {
        if (access(c.c_str(), R_OK) == 0) return c;
    }
    return "";
}

static void frame_stats_init(Window *root);

extern "C" int ghost_load(void)
{
    std::string dir = exe_dir();
    std::string lib = find_file("AVP2_LIBGODOT", "libgodot", { dir + "/" LIBGODOT_NAME, LIBGODOT_NAME });
    std::string project = find_file("AVP2_GODOT_PROJECT", "godot_project", { dir + "/godot/project.godot", "godot/project.godot" });

    if (lib.empty() || project.empty())
    {
        if (winapi_debug) eprintf("host: no %s\n", lib.empty() ? LIBGODOT_NAME : "Godot project (godot/project.godot)");
        return 0;
    }
    if ((project.size() > 14) && (project.compare(project.size() - 14, 14, "/project.godot") == 0)) project.resize(project.size() - 14);

    libgodot = dlopen(lib.c_str(), RTLD_NOW | RTLD_LOCAL | RTLD_DEEPBIND);
    if (libgodot == NULL)
    {
        eprintf("host: %s\n", dlerror());
        return 0;
    }
    create_instance = (create_instance_fn)dlsym(libgodot, "libgodot_create_godot_instance");
    destroy_instance = (destroy_instance_fn)dlsym(libgodot, "libgodot_destroy_godot_instance");
    if (create_instance == NULL)
    {
        eprintf("host: %s has no libgodot_create_godot_instance\n", lib.c_str());
        return 0;
    }

    static std::string res = "640x480";
    std::vector<std::string> args = { "sr-avp2", "--path", project, "--resolution", res, "--audio-driver", "Dummy", "--disable-crash-handler" };
    const char *driver = config_get("rendering_driver");
    if (driver != NULL) { args.push_back("--rendering-driver"); args.push_back(driver); }
    // forward_plus (the project's), mobile or gl_compatibility (needed with the opengl3 driver)
    const char *method = config_get("rendering_method");
    if (method != NULL) { args.push_back("--rendering-method"); args.push_back(method); }
    if (winapi_debug >= 3) args.push_back("--verbose");
    static std::vector<std::string> keep;
    keep = args;
    static std::vector<char *> argv;
    argv.clear();
    for (std::string &a : keep) argv.push_back((char *)a.c_str());
    argv.push_back(NULL);

    // Godot makes the project directory the current one (--path); the game runs in its own
    char cwd[PATH_MAX];
    if (getcwd(cwd, sizeof(cwd)) == NULL) cwd[0] = 0;

    instance_ptr = create_instance((int)keep.size(), argv.data(), avp2_godot_init);
    if (instance_ptr == NULL)
    {
        eprintf("host: libgodot_create_godot_instance failed\n");
        return 0;
    }
    instance = reinterpret_cast<GodotInstance *>(internal::get_object_instance_binding(instance_ptr));
    if ((instance == NULL) || !instance->start())
    {
        eprintf("host: Godot didn't start\n");
        return 0;
    }
    started = 1;
    main_thread = pthread_self();
    if ((cwd[0] != 0) && (chdir(cwd) != 0)) eprintf("host: can't return to %s\n", cwd);

    SceneTree *tree = Object::cast_to<SceneTree>(Engine::get_singleton()->get_main_loop());
    tree->set_auto_accept_quit(false);
    Window *root = tree->get_root();
    root->add_child(memnew(AvP2Host));
    frame_stats_init(root);

    RenderingServer *rs = RenderingServer::get_singleton();
    gs = new godot_state;
    gs->canvas = rs->canvas_create();
    rs->viewport_attach_canvas(root->get_viewport_rid(), gs->canvas);
    gs->canvas_item = rs->canvas_item_create();
    rs->canvas_item_set_parent(gs->canvas_item, gs->canvas);
    gs->shader = rs->shader_create();
    rs->shader_set_code(gs->shader, "shader_type canvas_item;\nrender_mode blend_premul_alpha;\nvoid fragment() { vec4 c = texture(TEXTURE, UV); COLOR = vec4(c.b, c.g, c.r, c.a); }\n");
    gs->material = rs->material_create();
    rs->material_set_shader(gs->material, gs->shader);
    rs->canvas_item_set_material(gs->canvas_item, gs->material);

    eprintf("host: Godot %s (%s)\n", lib.c_str(), project.c_str());
    return 1;
}

/* ------------------------------------------------------------------ */
/* window                                                              */

static void run_iteration(void)
{
    if (!started || quitting) return;
    apply_wanted();
    last_iteration = SDL_GetTicks();
    if (instance->iteration())
    {
        SDL_Event ev;
        memset(&ev, 0, sizeof(ev));
        ev.type = SDL_QUIT;
        push_event(ev);
    }
}

extern "C" void ghost_resize(int w, int h);
extern "C" void ghost_set_title(const char *title);

extern "C" int ghost_create_window(const char *title, int w, int h)
{
    if (!started) return 0;
    if (config_get_int("fullscreen", 0) && on_main_thread()) DisplayServer::get_singleton()->window_set_mode(DisplayServer::WINDOW_MODE_FULLSCREEN);
    ghost_resize(w, h);
    ghost_set_title(title);
    return 1;
}

extern "C" void ghost_destroy_window(void) {}
extern "C" int ghost_window_exists(void) { return started; }

extern "C" void ghost_set_title(const char *title)
{
    if (title == NULL) return;
    pthread_mutex_lock(&pending_mutex);
    want.title = title;
    want.set_title = 1;
    pthread_mutex_unlock(&pending_mutex);
    if (started && on_main_thread()) apply_wanted();
}

// the game sets its window's client size (render mode): the window follows (times window_scale) unless it's
// full screen, where the picture is scaled to the screen
extern "C" void ghost_resize(int w, int h)
{
    pthread_mutex_lock(&pending_mutex);
    want.size_w = w;
    want.size_h = h;
    want.size = 1;
    pthread_mutex_unlock(&pending_mutex);
    if (started && on_main_thread()) apply_wanted();
}

static void present_frame(const uint32_t *pixels, int w, int h);

extern "C" void ghost_present(const uint32_t *pixels, int w, int h)
{
    if (!started) return;
    if (!on_main_thread())
    {
        pthread_mutex_lock(&pending_mutex);
        pending_pixels.assign(pixels, pixels + (size_t)w * h);
        pending_w = w;
        pending_h = h;
        pending = 1;
        pthread_mutex_unlock(&pending_mutex);
        return;
    }
    present_frame(pixels, w, h);
}

// the latest frame from another thread, on the main thread
static int present_pending(void)
{
    std::vector<uint32_t> px;
    int w, h;
    pthread_mutex_lock(&pending_mutex);
    if (!pending) { pthread_mutex_unlock(&pending_mutex); return 0; }
    px.swap(pending_pixels);
    w = pending_w;
    h = pending_h;
    pending = 0;
    pthread_mutex_unlock(&pending_mutex);
    present_frame(px.data(), w, h);
    return 1;
}

/* ------------------------------------------------------------------ */
/* frame timing                                                        */

// Every presented game frame: the interval since the previous one, split into the game's own work (from the end
// of the previous present to this one), the 2D frame's upload and the Godot frame (iteration: 3D + 2D drawing,
// buffer swap). Per second: frames, interval average / percentiles / worst and hitches (> 25 ms, > 50 ms), with
// the worst frame's split. AVP2_FRAMESTATS=1 logs it, show_fps=1 (or AVP2_SHOW_FPS=1) shows it on screen.
// SDL's performance counter (portable).
// Godot's pipeline compilations (RenderingServer totals): the ones made while drawing block the frame (a shader or
// vertex format used for the first time: stutter), the surface ones are started when an object is added and waited
// for when it's drawn in the same frame, specializations are in the background. Logged per second and with each slow
// frame (> 25 ms in Godot or > 50 ms in the game).
enum { PC_CANVAS, PC_MESH, PC_SURFACE, PC_DRAW, PC_SPEC, PC_N };

static struct
{
    int log, show;
    uint64_t prev_start, prev_end, window_start;
    std::vector<float> interval, game, upload, godot;
    uint64_t pc_prev[PC_N], pc_window[PC_N];
    Label *label;
} fst;

static double perf_ms(uint64_t a, uint64_t b) { return (double)(b - a) * 1000.0 / (double)SDL_GetPerformanceFrequency(); }

static void pipeline_counts(uint64_t c[PC_N])
{
    static const RenderingServer::RenderingInfo info[PC_N] = { RenderingServer::RENDERING_INFO_PIPELINE_COMPILATIONS_CANVAS,
        RenderingServer::RENDERING_INFO_PIPELINE_COMPILATIONS_MESH, RenderingServer::RENDERING_INFO_PIPELINE_COMPILATIONS_SURFACE,
        RenderingServer::RENDERING_INFO_PIPELINE_COMPILATIONS_DRAW, RenderingServer::RENDERING_INFO_PIPELINE_COMPILATIONS_SPECIALIZATION };
    RenderingServer *rs = RenderingServer::get_singleton();
    for (int i = 0; i < PC_N; i++) c[i] = rs->get_rendering_info(info[i]);
}

static void frame_stats_init(Window *root)
{
    fst.log = (getenv("AVP2_FRAMESTATS") != NULL) && (atoi(getenv("AVP2_FRAMESTATS")) != 0);
    fst.show = getenv("AVP2_SHOW_FPS") ? (atoi(getenv("AVP2_SHOW_FPS")) != 0) : (config_get_int("show_fps", 0) != 0);
    if (!fst.show) return;
    CanvasLayer *layer = memnew(CanvasLayer);
    layer->set_layer(100);
    fst.label = memnew(Label);
    fst.label->add_theme_color_override("font_color", Color(1.0f, 1.0f, 0.4f));
    fst.label->add_theme_color_override("font_outline_color", Color(0, 0, 0));
    fst.label->add_theme_constant_override("outline_size", 4);
    fst.label->add_theme_font_size_override("font_size", 14);
    fst.label->set_horizontal_alignment(HORIZONTAL_ALIGNMENT_CENTER);
    layer->add_child(fst.label);
    root->add_child(layer);
}

static void frame_stats_add(uint64_t start, uint64_t uploaded, uint64_t end)
{
    if (!fst.log && !fst.show) return;
    uint64_t pc[PC_N];
    pipeline_counts(pc);
    if (fst.prev_start != 0)
    {
        fst.interval.push_back((float)perf_ms(fst.prev_start, start));
        fst.game.push_back((float)perf_ms(fst.prev_end, start));
        fst.upload.push_back((float)perf_ms(start, uploaded));
        fst.godot.push_back((float)perf_ms(uploaded, end));
        if (fst.log && ((fst.godot.back() > 25.0f) || (fst.game.back() > 50.0f)))
            eprintf("frames: slow frame: game %.1f upload %.1f godot %.1f ms; pipelines draw +%d surface +%d mesh +%d specialization +%d canvas +%d\n",
                    fst.game.back(), fst.upload.back(), fst.godot.back(), (int)(pc[PC_DRAW] - fst.pc_prev[PC_DRAW]), (int)(pc[PC_SURFACE] - fst.pc_prev[PC_SURFACE]),
                    (int)(pc[PC_MESH] - fst.pc_prev[PC_MESH]), (int)(pc[PC_SPEC] - fst.pc_prev[PC_SPEC]), (int)(pc[PC_CANVAS] - fst.pc_prev[PC_CANVAS]));
    }
    else
    {
        fst.window_start = start;
        memcpy(fst.pc_window, pc, sizeof(pc));
    }
    memcpy(fst.pc_prev, pc, sizeof(pc));
    fst.prev_start = start;
    fst.prev_end = end;
    double window = perf_ms(fst.window_start, end);
    if ((window < 1000.0) || fst.interval.empty()) return;

    size_t n = fst.interval.size(), worst = 0;
    double sum = 0, gsum = 0, usum = 0, osum = 0;
    int over25 = 0, over50 = 0;
    for (size_t i = 0; i < n; i++)
    {
        sum += fst.interval[i]; gsum += fst.game[i]; usum += fst.upload[i]; osum += fst.godot[i];
        if (fst.interval[i] > fst.interval[worst]) worst = i;
        if (fst.interval[i] > 25.0f) over25++;
        if (fst.interval[i] > 50.0f) over50++;
    }
    std::vector<float> sorted = fst.interval;
    std::sort(sorted.begin(), sorted.end());
    float p50 = sorted[n / 2], p95 = sorted[std::min(n - 1, n * 95 / 100)], p99 = sorted[std::min(n - 1, n * 99 / 100)];
    double fps = n * 1000.0 / window;
    if (fst.log)
        eprintf("frames: %.1f fps, interval avg %.2f p50 %.2f p95 %.2f p99 %.2f max %.2f ms, >25 ms %d, >50 ms %d; "
                "avg game %.2f upload %.2f godot %.2f; worst: game %.2f upload %.2f godot %.2f; pipelines draw +%d surface +%d "
                "specialization +%d (total draw %d surface %d)\n",
                fps, sum / n, p50, p95, p99, sorted[n - 1], over25, over50, gsum / n, usum / n, osum / n,
                fst.game[worst], fst.upload[worst], fst.godot[worst], (int)(pc[PC_DRAW] - fst.pc_window[PC_DRAW]),
                (int)(pc[PC_SURFACE] - fst.pc_window[PC_SURFACE]), (int)(pc[PC_SPEC] - fst.pc_window[PC_SPEC]), (int)pc[PC_DRAW], (int)pc[PC_SURFACE]);
    if (fst.label != NULL)
    {
        char text[256];
        snprintf(text, sizeof(text), "%.0f fps  %.1f ms (p99 %.1f, max %.1f)  hitches %d\ngame %.1f  upload %.1f  godot %.1f ms",
                 fps, sum / n, p99, sorted[n - 1], over25, gsum / n, usum / n, osum / n);
        fst.label->set_text(text);
        // top centre: between the HUD's health (left) and ammunition (right)
        Vector2 ms = fst.label->get_minimum_size();
        Vector2i ws = DisplayServer::get_singleton()->window_get_size();
        fst.label->set_position(Vector2(floorf((ws.x - ms.x) * 0.5f), 2));
    }
    fst.interval.clear(); fst.game.clear(); fst.upload.clear(); fst.godot.clear();
    fst.window_start = end;
    memcpy(fst.pc_window, pc, sizeof(pc));
}

static void present_frame(const uint32_t *pixels, int w, int h)
{
    RenderingServer *rs = RenderingServer::get_singleton();
    uint64_t t_start = SDL_GetPerformanceCounter();

    if (gs->frame_bytes.size() != (int64_t)w * h * 4) gs->frame_bytes.resize((int64_t)w * h * 4);
    memcpy(gs->frame_bytes.ptrw(), pixels, (size_t)w * h * 4);
    if (gs->frame_image.is_null()) gs->frame_image.instantiate();
    gs->frame_image->set_data(w, h, false, Image::FORMAT_RGBA8, gs->frame_bytes);
    if (!gs->texture.is_valid() || (w != tex_w) || (h != tex_h))
    {
        if (gs->texture.is_valid()) rs->free_rid(gs->texture);
        gs->texture = rs->texture_2d_create(gs->frame_image);
        tex_w = w;
        tex_h = h;
    }
    else
    {
        rs->texture_2d_update(gs->texture, gs->frame_image, 0);
    }

    Vector2i size = DisplayServer::get_singleton()->window_get_size();
    rs->canvas_item_clear(gs->canvas_item);
    rs->canvas_item_add_texture_rect(gs->canvas_item, Rect2(0, 0, size.x, size.y), gs->texture);

    uint64_t t_uploaded = SDL_GetPerformanceCounter();
    run_iteration();
    frame_stats_add(t_start, t_uploaded, SDL_GetPerformanceCounter());

    // debugging: AVP2_DUMP_GODOT=<existing dir> saves what Godot drew (read back from the GPU) every 2 seconds
    static uint32_t last_dump;
    static int dump_index;
    const char *dir = getenv("AVP2_DUMP_GODOT");
    if ((dir != NULL) && started && (SDL_GetTicks() - last_dump >= 2000))
    {
        last_dump = SDL_GetTicks();
        SceneTree *tree = Object::cast_to<SceneTree>(Engine::get_singleton()->get_main_loop());
        Ref<Image> shot = tree->get_root()->get_texture()->get_image();
        if (shot.is_valid())
        {
            char name[1024];
            snprintf(name, sizeof(name), "%s/godot%04d.png", dir, dump_index++);
            shot->save_png(name);
            if (winapi_debug >= 2)
            {
                Vector2i ws = DisplayServer::get_singleton()->window_get_size();
                eprintf("host: dump %s: window %dx%d, viewport %dx%d, requested %dx%d\n", name, ws.x, ws.y, shot->get_width(), shot->get_height(), req_w, req_h);
            }
        }
    }
}

extern "C" void ghost_pump(void)
{
    if (!started || !on_main_thread()) return;
    if (present_pending()) return;
    // the game is loading or only processing messages: a Godot frame now and then keeps the window alive
    if (SDL_GetTicks() - last_iteration >= 100) run_iteration();
}

/* ------------------------------------------------------------------ */
/* input                                                               */

extern "C" int ghost_poll_event(SDL_Event *ev)
{
    if (queue_count == 0) return 0;
    *ev = queue[queue_head];
    queue_head = (queue_head + 1) % (int)(sizeof(queue) / sizeof(queue[0]));
    queue_count--;
    return 1;
}

extern "C" const uint8_t *ghost_keyboard_state(void) { return keys; }

extern "C" uint32_t ghost_mouse_state(int *x, int *y)
{
    if (x != NULL) *x = mouse_x;
    if (y != NULL) *y = mouse_y;
    return mouse_buttons;
}

extern "C" void ghost_window_size(int *w, int *h)
{
    if (!started) { *w = 0; *h = 0; return; }
    Vector2i size = DisplayServer::get_singleton()->window_get_size();
    *w = size.x;
    *h = size.y;
}

extern "C" void ghost_warp_mouse(int wx, int wy)
{
    pthread_mutex_lock(&pending_mutex);
    want.warp_x = wx;
    want.warp_y = wy;
    want.warp = 1;
    pthread_mutex_unlock(&pending_mutex);
    mouse_x = wx;
    mouse_y = wy;
    if (started && on_main_thread()) apply_wanted();
}

static void update_mouse_mode(void)
{
    pthread_mutex_lock(&pending_mutex);
    want.mouse_mode = 1;
    pthread_mutex_unlock(&pending_mutex);
    if (started && on_main_thread()) apply_wanted();
}

extern "C" void ghost_set_relative_mouse(int on)
{
    relative_mouse = on;
    update_mouse_mode();
}

extern "C" void ghost_show_cursor(int show)
{
    cursor_hidden = !show;
    update_mouse_mode();
}

extern "C" void ghost_toggle_fullscreen(void)
{
    pthread_mutex_lock(&pending_mutex);
    want.toggle_fullscreen = 1;
    pthread_mutex_unlock(&pending_mutex);
    if (started && on_main_thread()) apply_wanted();
}

extern "C" void ghost_desktop_size(int *w, int *h)
{
    *w = 1920;
    *h = 1080;
    if (!started) return;
    Vector2i size = DisplayServer::get_singleton()->screen_get_size();
    if ((size.x > 0) && (size.y > 0)) { *w = size.x; *h = size.y; }
}

/* ------------------------------------------------------------------ */
/* cursors                                                             */

struct host_cursor
{
    Ref<Image> image;           // null: a system cursor
    int shape;
    Vector2 hotspot;
};

extern "C" void *ghost_create_cursor(const uint32_t *argb, int w, int h, int hot_x, int hot_y)
{
    if (!started) return NULL;
    PackedByteArray b;
    b.resize((int64_t)w * h * 4);
    uint8_t *d = b.ptrw();
    for (int i = 0; i < w * h; i++)
    {
        d[4 * i + 0] = (uint8_t)(argb[i] >> 16);
        d[4 * i + 1] = (uint8_t)(argb[i] >> 8);
        d[4 * i + 2] = (uint8_t)argb[i];
        d[4 * i + 3] = (uint8_t)(argb[i] >> 24);
    }
    host_cursor *c = new host_cursor;
    c->image = Image::create_from_data(w, h, false, Image::FORMAT_RGBA8, b);
    c->shape = Input::CURSOR_ARROW;
    c->hotspot = Vector2(hot_x, hot_y);
    return c;
}

extern "C" void *ghost_system_cursor(int id)
{
    host_cursor *c = new host_cursor;
    switch (id)
    {
        case SDL_SYSTEM_CURSOR_IBEAM: c->shape = Input::CURSOR_IBEAM; break;
        case SDL_SYSTEM_CURSOR_WAIT: c->shape = Input::CURSOR_WAIT; break;
        case SDL_SYSTEM_CURSOR_CROSSHAIR: c->shape = Input::CURSOR_CROSS; break;
        case SDL_SYSTEM_CURSOR_WAITARROW: c->shape = Input::CURSOR_BUSY; break;
        case SDL_SYSTEM_CURSOR_SIZENWSE: c->shape = Input::CURSOR_FDIAGSIZE; break;
        case SDL_SYSTEM_CURSOR_SIZENESW: c->shape = Input::CURSOR_BDIAGSIZE; break;
        case SDL_SYSTEM_CURSOR_SIZEWE: c->shape = Input::CURSOR_HSIZE; break;
        case SDL_SYSTEM_CURSOR_SIZENS: c->shape = Input::CURSOR_VSIZE; break;
        case SDL_SYSTEM_CURSOR_SIZEALL: c->shape = Input::CURSOR_MOVE; break;
        case SDL_SYSTEM_CURSOR_NO: c->shape = Input::CURSOR_FORBIDDEN; break;
        case SDL_SYSTEM_CURSOR_HAND: c->shape = Input::CURSOR_POINTING_HAND; break;
        default: c->shape = Input::CURSOR_ARROW; break;
    }
    return c;
}

extern "C" void ghost_set_cursor(void *cursor)
{
    if (cursor == NULL) return;
    pthread_mutex_lock(&pending_mutex);
    want.cursor = cursor;
    want.set_cursor = 1;
    pthread_mutex_unlock(&pending_mutex);
    if (started && on_main_thread()) apply_wanted();
}

static void apply_cursor(void *cursor)
{
    host_cursor *c = (host_cursor *)cursor;
    Input *in = Input::get_singleton();
    if (c->image.is_valid())
    {
        in->set_custom_mouse_cursor(c->image, Input::CURSOR_ARROW, c->hotspot);
        in->set_default_cursor_shape(Input::CURSOR_ARROW);
    }
    else
    {
        in->set_custom_mouse_cursor(Ref<Resource>(), Input::CURSOR_ARROW, Vector2());
        in->set_default_cursor_shape((Input::CursorShape)c->shape);
    }
}

static void apply_wanted(void)
{
    if (!started) return;
    pthread_mutex_lock(&pending_mutex);
    auto w = want;
    want.size = want.set_title = want.mouse_mode = want.set_cursor = want.toggle_fullscreen = want.warp = 0;
    pthread_mutex_unlock(&pending_mutex);

    DisplayServer *ds = DisplayServer::get_singleton();
    if (w.set_title) ds->window_set_title(String::utf8(w.title.c_str()));
    if (w.toggle_fullscreen)
    {
        DisplayServer::WindowMode m = ds->window_get_mode();
        ds->window_set_mode((m == DisplayServer::WINDOW_MODE_FULLSCREEN || m == DisplayServer::WINDOW_MODE_EXCLUSIVE_FULLSCREEN) ? DisplayServer::WINDOW_MODE_WINDOWED : DisplayServer::WINDOW_MODE_FULLSCREEN);
    }
    if (w.size)
    {
        // the game sets its window's client size (render mode): the window follows (times window_scale) unless
        // it's full screen or maximized, where the picture is scaled to it
        DisplayServer::WindowMode m = ds->window_get_mode();
        if ((m != DisplayServer::WINDOW_MODE_FULLSCREEN) && (m != DisplayServer::WINDOW_MODE_EXCLUSIVE_FULLSCREEN) && (m != DisplayServer::WINDOW_MODE_MAXIMIZED))
        {
            int scale = config_get_int("window_scale", 1);
            if (scale < 1) scale = 1;
            req_w = w.size_w * scale;
            req_h = w.size_h * scale;
            req_time = SDL_GetTicks();
            ds->window_set_size(Vector2i(req_w, req_h));
        }
    }
    else if ((req_w > 0) && (SDL_GetTicks() - req_time < 2000) && (ds->window_get_mode() == DisplayServer::WINDOW_MODE_WINDOWED))
    {
        // X11 resizes asynchronously: after two quick requests (the engine's initial 320x200, then the render mode)
        // Godot can be left with the window at the new size but the root viewport at the old one (no resize event
        // for the second) - ask again, or nudge the size by a pixel, for a while (later the user may resize)
        Vector2i s = ds->window_get_size();
        SceneTree *tree = Object::cast_to<SceneTree>(Engine::get_singleton()->get_main_loop());
        Vector2i v = (tree != NULL) ? tree->get_root()->get_size() : s;
        if ((s.x != req_w) || (s.y != req_h)) ds->window_set_size(Vector2i(req_w, req_h));
        else if ((v.x != req_w) || (v.y != req_h)) ds->window_set_size(Vector2i(req_w, req_h + 1));
    }
    if (w.mouse_mode)
        Input::get_singleton()->set_mouse_mode(relative_mouse ? Input::MOUSE_MODE_CAPTURED : (cursor_hidden ? Input::MOUSE_MODE_HIDDEN : Input::MOUSE_MODE_VISIBLE));
    if (w.set_cursor) apply_cursor(w.cursor);
    if (w.warp) Input::get_singleton()->warp_mouse(Vector2(w.warp_x, w.warp_y));
}

extern "C" void ghost_shutdown(void)
{
    if (!started) return;
    quitting = 1;
    // the process ends right after this: Godot's own shutdown (Vulkan device, window) is all that matters
    if (destroy_instance != NULL) destroy_instance(instance_ptr);
    started = 0;
}
