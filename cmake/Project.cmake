# project.json, read before project(): FRY_VERSION and the rest as FRY_<KEY>; fry_project_header()
# writes them as C++ constants (fry_project.h) and Windows' file details (fry3dx.rc).
set(FRY_PROJECT_JSON ${CMAKE_CURRENT_LIST_DIR}/../project.json)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${FRY_PROJECT_JSON})
file(READ ${FRY_PROJECT_JSON} FRY_JSON)
foreach(key name long version tagline description website repo issues releases license copyright company look)
  string(TOUPPER ${key} upper)
  string(JSON FRY_${upper} GET "${FRY_JSON}" ${key})
endforeach()

# out: the header's path; rc: Windows' file details (the version as 0,8,1,0 too).
function(fry_project_header out rc)
  set(h "// Made by CMake from project.json: change that, not this.\n#pragma once\n#include <span>\n\nnamespace fry::project {\n")
  foreach(key name long version tagline description website repo issues releases license copyright company look)
    string(TOUPPER ${key} upper)
    set(ident ${key})
    if(key STREQUAL "long")
      set(ident long_) # (a C++ word)
    endif()
    string(APPEND h "inline constexpr const char* ${ident} = R\"fry(${FRY_${upper}})fry\";\n")
  endforeach()
  string(APPEND h "\nstruct Creator {\n    const char* name;\n    const char* role;\n    const char* url;\n};\ninline constexpr Creator kCreators[] = {\n")
  string(JSON n LENGTH "${FRY_JSON}" creators)
  math(EXPR last "${n} - 1")
  foreach(i RANGE ${last})
    string(JSON who GET "${FRY_JSON}" creators ${i} name)
    string(JSON role ERROR_VARIABLE none GET "${FRY_JSON}" creators ${i} role)
    string(JSON url ERROR_VARIABLE none GET "${FRY_JSON}" creators ${i} url)
    if(role MATCHES "NOTFOUND$")
      set(role "")
    endif()
    if(url MATCHES "NOTFOUND$")
      set(url "")
    endif()
    string(APPEND h "    {R\"fry(${who})fry\", R\"fry(${role})fry\", R\"fry(${url})fry\"},\n")
  endforeach()
  string(APPEND h "};\ninline constexpr std::span<const Creator> creators{kCreators};\n\n// What it's built on (for the About box).\ninline constexpr const char* builtWith = R\"fry(")
  string(JSON n LENGTH "${FRY_JSON}" builtWith)
  math(EXPR last "${n} - 1")
  foreach(i RANGE ${last})
    string(JSON part GET "${FRY_JSON}" builtWith ${i})
    if(i GREATER 0)
      if(i EQUAL last)
        string(APPEND h " and ")
      else()
        string(APPEND h ", ")
      endif()
    endif()
    string(APPEND h "${part}")
  endforeach()
  string(APPEND h ")fry\";\n} // namespace fry::project\n")
  file(CONFIGURE OUTPUT ${out} CONTENT "${h}" @ONLY)

  string(REPLACE "." "," commas "${FRY_VERSION}")
  file(CONFIGURE OUTPUT ${rc} CONTENT "// Made by CMake from project.json (Windows: the program's icon and details, Explorer's Properties).
1 ICON \"${PROJECT_SOURCE_DIR}/branding/fry3dx.ico\"
1 VERSIONINFO
FILEVERSION ${commas},0
PRODUCTVERSION ${commas},0
BEGIN
  BLOCK \"StringFileInfo\"
  BEGIN
    BLOCK \"040904b0\"
    BEGIN
      VALUE \"CompanyName\", \"${FRY_COMPANY}\"
      VALUE \"FileDescription\", \"${FRY_NAME} - ${FRY_LONG}\"
      VALUE \"FileVersion\", \"${FRY_VERSION}\"
      VALUE \"InternalName\", \"fry3dx\"
      VALUE \"LegalCopyright\", \"${FRY_COPYRIGHT}, ${FRY_LICENSE}\"
      VALUE \"OriginalFilename\", \"fry3dx.exe\"
      VALUE \"ProductName\", \"${FRY_NAME}\"
      VALUE \"ProductVersion\", \"${FRY_VERSION}\"
    END
  END
  BLOCK \"VarFileInfo\"
  BEGIN
    VALUE \"Translation\", 0x409, 1200
  END
END
" @ONLY)
endfunction()
