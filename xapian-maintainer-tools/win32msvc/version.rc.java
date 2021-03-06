#include "winres.h"
#include "xapian\version.h"
#include "javaversion.h"

VS_VERSION_INFO VERSIONINFO
 FILEVERSION XAPIAN_MAJOR_VERSION,XAPIAN_MINOR_VERSION,XAPIAN_REVISION
 PRODUCTVERSION XAPIAN_MAJOR_VERSION,XAPIAN_MINOR_VERSION,XAPIAN_REVISION
 FILEFLAGSMASK 0x3fL
#ifdef _DEBUG
 FILEFLAGS VS_FF_DEBUG
#else
 FILEFLAGS 0x0L
#endif
 FILEOS VOS__WINDOWS32
 FILETYPE VFT_DLL
BEGIN
    BLOCK "StringFileInfo"
    BEGIN
        BLOCK "040904b0"
        BEGIN
            VALUE "ProductName", "Xapian"
            VALUE "ProductVersion", XAPIAN_VERSION
            VALUE "FileDescription", "Xapian " XAPIAN_VERSION " bindings for Java " JAVA_VERSION
            VALUE "LegalCopyright", "Consult source code for copyright information"
            VALUE "FileVersion", XAPIAN_VERSION
            VALUE "URL", "http://www.xapian.org/"
            VALUE "LegalTrademarks", "Xapian is an Open Source Search Engine Library released under the GPL."
        END
    END
END 
