@echo off
rem Build the repro with debug info and no optimization so line mapping is clean
rem for vs-debug-mcp / Visual Studio native debugging.
cl /nologo /EHsc /Zi /Od repro.cpp /Fe:repro.exe /Fdrepro.pdb
