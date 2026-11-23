#! /usr/bin/python3
# Replaces repeated lines "db N" and "resb 1" in generated files seg*.inc with "times" / "resb" directives.

import glob
import os

def Compact(cFile):
    cTmpFile = cFile + "tmp"
    fIn = open(cFile, "rt")
    fOut = open(cTmpFile, "wt")

    iNum = 0
    cRepStr = ""

    def Flush():
        if iNum == 1:
            fOut.write(cRepStr)
        elif cRepStr.startswith("resb"):
            fOut.write("resb " + str(iNum) + "\n")
        elif iNum == 2:
            fOut.write(cRepStr)
            fOut.write(cRepStr)
        else:
            fOut.write("times " + str(iNum) + " " + cRepStr)

    for cLine in fIn:
        if iNum != 0:
            if cLine == cRepStr:
                iNum = iNum + 1
                continue
            Flush()
            iNum = 0

        if cLine.startswith("db ") or cLine.strip() == "resb 1":
            iNum = 1
            cRepStr = cLine
        else:
            fOut.write(cLine)

    if iNum != 0:
        Flush()

    fOut.close()
    fIn.close()

    os.remove(cFile)
    os.rename(cTmpFile, cFile)

for cFile in sorted(glob.glob("seg*.inc")):
    Compact(cFile)
