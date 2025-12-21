#include "stdafx.h"
#include "MainDlgConfig.h"

// Categories
const TCHAR* kCategories[] = {
    _T("General"),
    _T("Appearance"),
    _T("Interaction"),
    _T("Image"),
    _T("Misc"),
    _T("Shortcuts")
};

// We need to implement OnInitDialog here properly to create the splitter
// Since I put the handler in the header, I should move the logic to the header or keep it simpler.
// Actually, I replaced the class definition in the header with a partial one.
// The previous `write_to_file` for cpp was empty.

// Let's keep the logic inline in header for now as WTL often does, 
// OR move functions to CPP.
// I will just modify the header to include the logic.
// The CPP file will just be an include stub for now.
