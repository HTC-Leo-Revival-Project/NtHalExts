@echo off
cl /O1 /I . /std:c17 /D _ARM_=1 /GS- halextmsmvic.c halextapi.c /link /DRIVER /DLL /SUBSYSTEM:native /OUT:halextmsmvic.dll /ENTRY:HalExtensionInit /NODEFAULTLIB ntoskrnl.lib