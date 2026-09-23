#! /bin/sh

cd "`dirname "$0"`"

FILE_RESULT=

find_file () {
    if [ -e "$1/$2" ]
    then
        FILE_RESULT="$2"
        return
    fi

    TEMP_FILE_UPPER=`echo "$2" | tr '[:lower:]' '[:upper:]'`

    if [ -e "$1/$TEMP_FILE_UPPER" ]
    then
        FILE_RESULT="$TEMP_FILE_UPPER"
        return
    fi

    TEMP_FILE_LOWER=`echo "$2" | tr '[:upper:]' '[:lower:]'`

    if [ -e "$1/$TEMP_FILE_LOWER" ]
    then
        FILE_RESULT="$TEMP_FILE_LOWER"
        return
    fi

    FILE_RESULT=
}


find_file "." "i76.zfs"

if [ -z "$FILE_RESULT" ]
then
    echo "Interstate '76 game not found"
    zenity --error --text="Interstate '76 game not found" --timeout=10 2>/dev/null

    exit 1
fi

export LD_LIBRARY_PATH="`pwd`"

./SR-I76 "$@"
sync
