# patch_dep: a patched copy of a vendored dependency, made at configure time.
#
#   patch_dep(SOURCE  ${CMAKE_SOURCE_DIR}/deps/Raven
#             BINARY  ${CMAKE_BINARY_DIR}/patched/Raven
#             EXCLUDE bindings docs tools          # top-level entries not copied
#             PATCHES ${CMAKE_SOURCE_DIR}/patch/raven-foo.patch ...)
#
# deps/ stays exactly as cdeps vendored it; the build compiles the copy in
# BINARY instead (add_subdirectory(<BINARY> ...)). The copy is remade only
# when a patch, deps.lock or this script changes, so configuring again does
# not touch the patched files and recompile them.
#
# Patches are unified diffs (git diff / diff -u, paths a/... b/...). Applying
# needs no patch or git program: each hunk's old text (context and removed
# lines) must occur exactly once in its file and is replaced by its new text.
# Line numbers are ignored, so a patch survives unrelated upstream changes; a
# hunk that no longer matches stops the configure.

set(_PATCH_DEP_SCRIPT "${CMAKE_CURRENT_LIST_FILE}")

function(_patch_dep_write_file root file content)
  if(file)
    file(WRITE "${root}/${file}" "${content}")
  endif()
endfunction()

function(_patch_dep_apply root patch)
  file(READ "${patch}" text)
  get_filename_component(patch_name "${patch}" NAME)
  set(file "")
  set(content "")
  set(hunk 0)
  set(old_left 0)
  set(new_left 0)
  set(old "")
  set(new "")
  while(NOT text STREQUAL "")
    # Next line, without its newline
    string(FIND "${text}" "\n" nl)
    if(nl EQUAL -1)
      set(line "${text}")
      set(text "")
    else()
      string(SUBSTRING "${text}" 0 ${nl} line)
      math(EXPR nl "${nl} + 1")
      string(SUBSTRING "${text}" ${nl} -1 text)
    endif()
    string(REGEX REPLACE "\r$" "" line "${line}")

    if(old_left GREATER 0 OR new_left GREATER 0)
      # Inside a hunk: the counts say where it ends, so a "+++" or "---"
      # line here is content, not a file header.
      string(SUBSTRING "${line}" 0 1 tag)
      string(LENGTH "${line}" length)
      if(length GREATER 0)
        string(SUBSTRING "${line}" 1 -1 body)
      else()
        set(body "") # an empty context line with its space trimmed
      endif()
      if(tag STREQUAL "+")
        string(APPEND new "${body}\n")
        math(EXPR new_left "${new_left} - 1")
      elseif(tag STREQUAL "-")
        string(APPEND old "${body}\n")
        math(EXPR old_left "${old_left} - 1")
      elseif(tag STREQUAL "\\")
        # "\ No newline at end of file"
      else()
        string(APPEND old "${body}\n")
        string(APPEND new "${body}\n")
        math(EXPR old_left "${old_left} - 1")
        math(EXPR new_left "${new_left} - 1")
      endif()
      if(old_left LESS_EQUAL 0 AND new_left LESS_EQUAL 0)
        if(old STREQUAL "")
          message(FATAL_ERROR "${patch_name}: hunk ${hunk} of ${file} has no context to place it")
        endif()
        string(FIND "${content}" "${old}" first)
        string(FIND "${content}" "${old}" last REVERSE)
        if(first EQUAL -1)
          message(FATAL_ERROR "${patch_name}: hunk ${hunk} does not apply to ${file}")
        elseif(NOT first EQUAL last)
          message(FATAL_ERROR "${patch_name}: hunk ${hunk} matches ${file} in more than one place")
        endif()
        string(LENGTH "${old}" old_length)
        math(EXPR after "${first} + ${old_length}")
        string(SUBSTRING "${content}" 0 ${first} head)
        string(SUBSTRING "${content}" ${after} -1 tail)
        set(content "${head}${new}${tail}")
        set(old_left 0)
        set(new_left 0)
      endif()
    elseif(line MATCHES "^\\+\\+\\+ ([^\t]+)")
      _patch_dep_write_file("${root}" "${file}" "${content}")
      string(REGEX REPLACE "^b/" "" file "${CMAKE_MATCH_1}")
      if(NOT EXISTS "${root}/${file}")
        message(FATAL_ERROR "${patch_name}: ${file} is not in ${root}")
      endif()
      file(READ "${root}/${file}" content)
      set(hunk 0)
    elseif(line MATCHES "^@@ -[0-9]+(,([0-9]+))? \\+[0-9]+(,([0-9]+))? @@")
      if(file STREQUAL "")
        message(FATAL_ERROR "${patch_name}: a hunk before any +++ line")
      endif()
      set(old_left 1)
      set(new_left 1)
      if(NOT CMAKE_MATCH_2 STREQUAL "")
        set(old_left ${CMAKE_MATCH_2})
      endif()
      if(NOT CMAKE_MATCH_4 STREQUAL "")
        set(new_left ${CMAKE_MATCH_4})
      endif()
      math(EXPR hunk "${hunk} + 1")
      set(old "")
      set(new "")
    endif()
    # anything else (diff, index, --- lines) is a header and skipped
  endwhile()
  if(old_left GREATER 0 OR new_left GREATER 0)
    message(FATAL_ERROR "${patch_name}: hunk ${hunk} of ${file} is cut short")
  endif()
  _patch_dep_write_file("${root}" "${file}" "${content}")
endfunction()

function(patch_dep)
  cmake_parse_arguments(PARSE_ARGV 0 ARG "" "SOURCE;BINARY" "EXCLUDE;PATCHES")

  # What the copy is made from: the vendored version, the patches, this script
  set(inputs "${CMAKE_SOURCE_DIR}/deps.lock" ${ARG_PATCHES} "${_PATCH_DEP_SCRIPT}")
  set_property(DIRECTORY "${CMAKE_SOURCE_DIR}" APPEND PROPERTY
    CMAKE_CONFIGURE_DEPENDS ${inputs})
  set(key "${ARG_EXCLUDE}")
  foreach(input IN LISTS inputs)
    file(SHA256 "${input}" hash)
    string(APPEND key "${input}=${hash}\n")
  endforeach()
  string(SHA256 key "${key}")

  set(stamp "${ARG_BINARY}/.patch_dep_stamp")
  if(EXISTS "${stamp}")
    file(READ "${stamp}" done)
    if(done STREQUAL key)
      return()
    endif()
  endif()

  message(STATUS "Patching ${ARG_SOURCE} into ${ARG_BINARY}")
  file(REMOVE_RECURSE "${ARG_BINARY}")
  file(MAKE_DIRECTORY "${ARG_BINARY}")
  file(GLOB entries RELATIVE "${ARG_SOURCE}" "${ARG_SOURCE}/*")
  foreach(entry IN LISTS entries)
    if(NOT entry IN_LIST ARG_EXCLUDE)
      file(COPY "${ARG_SOURCE}/${entry}" DESTINATION "${ARG_BINARY}")
    endif()
  endforeach()
  foreach(patch IN LISTS ARG_PATCHES)
    _patch_dep_apply("${ARG_BINARY}" "${patch}")
  endforeach()
  file(WRITE "${stamp}" "${key}")
endfunction()
