# OmegaGSuite CMake Functions


# include(CMakeParseArguments)

set(CMAKE_C_STANDARD_REQUIRED YES)
set(CMAKE_C_STANDARD 11)

set(CMAKE_CXX_STANDARD_REQUIRED YES)
set(CMAKE_CXX_STANDARD 17)

	
set(PYEXEC)
if(WIN32)
    set(PYEXEC "py")
    find_program(PYTHON "py")
else()
    set(PYEXEC "python3")
    find_program(PYTHON "python3")
endif()

if(PYTHON)
    message("Found Python3")
	set(PYTHON TRUE CACHE INTERNAL "Python 3 has been found!")
else()
    message(FATAL_ERROR "Python 3 is NOT found...")
endif()



set(CODESIGN_SCRIPT ${CMAKE_CURRENT_LIST_DIR}/codesign.py)

# Captured at file-load time so functions defined here can reference the
# script regardless of which caller's `CMAKE_CURRENT_LIST_DIR` is in
# effect when the function body executes (function bodies expand
# `CMAKE_CURRENT_LIST_DIR` against the call-site listfile, not the
# definition site).
set(OMEGA_COPY_DLLS_SCRIPT ${CMAKE_CURRENT_LIST_DIR}/OmegaCopyDlls.cmake)



macro(add_script_target _NAME SCRIPT)
    cmake_parse_arguments("_ARG" "" "" "OUTPUTS;DEPS;ARGS" ${ARGN})
    add_custom_target(${_NAME} DEPENDS ${_ARG_OUTPUTS})
    add_custom_command(
        OUTPUT ${_ARG_OUTPUTS} 
        COMMAND ${PYEXEC} ${SCRIPT} ${_ARG_ARGS}
        DEPENDS ${_ARG_DEPS})
endmacro(add_script_target)

macro(get_target_output_names)
    cmake_parse_arguments("_ARG" "STATIC;SHARED;" "VAR" "TARGETS" ${ARGN})
    set(${_ARG_VAR})
    foreach(t ${_ARG_TARGETS})
        if(_ARG_STATIC)
            list(APPEND ${_ARG_VAR} $<TARGET_FILE:${t}>)
        elseif(_ARG_SHARED)
            list(APPEND ${_ARG_VAR} $<TARGET_FILE:${t}>)
        endif() 
    endforeach()

  
endmacro()


if(APPLE)
	if(NOT XCODE)
		set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)
	endif()
	if(NOT CODE_SIGNATURE)
		message(FATAL_ERROR "CODE_SIGNATURE Variable must be defined in order to sign Apple App and Framework Bundles. 
		Set the variable with your Apple Developer Team ID or goto apple.com and register with the Apple Developer Program")
	endif()
	
	# Strip version numbers from a dylib filename.
	# e.g. "libicuuc.69.1.dylib" -> "libicuuc.dylib"
	function(get_library_base_name FILENAME OUT_VAR)
		string(REGEX REPLACE "\\.[0-9]+(\\.[0-9]+)*\\.dylib$" ".dylib" _STRIPPED "${FILENAME}")
		set(${OUT_VAR} "${_STRIPPED}" PARENT_SCOPE)
	endfunction()

	macro(set_library_install_name LIB PATH)
		cmake_parse_arguments("_ILN" "" "" "AFTER" ${ARGN})
		get_filename_component(LIBNAME ${LIB} NAME)
		get_library_base_name("${LIBNAME}" LIBNAME_BASE)
		add_custom_target("${LIBNAME}_install_name"
			COMMAND install_name_tool -id ${PATH} ${LIB}
			COMMENT "Setting Install Name ${PATH} of Library ${LIBNAME_BASE}")
		if(_ILN_AFTER)
			add_dependencies("${LIBNAME}_install_name" ${_ILN_AFTER})
		endif()
		# Chain: subsequent operations on this library serialize after this one.
		set("_LAST_INSTALL_NAME_OP_${LIBNAME}" "${LIBNAME}_install_name")
	endmacro()

	macro(add_library_rpath LIB RPATH)
		cmake_parse_arguments("_ALR" "" "" "AFTER" ${ARGN})
		get_filename_component(LIBNAME ${LIB} NAME)
		get_library_base_name("${LIBNAME}" LIBNAME_BASE)
		set(_RPATH_SANITIZED "${RPATH}")
		string(REPLACE "/" "_" _RPATH_SANITIZED "${_RPATH_SANITIZED}")
		string(REPLACE "@" "_" _RPATH_SANITIZED "${_RPATH_SANITIZED}")
		add_custom_target("${LIBNAME}_${_RPATH_SANITIZED}_add_rpath"
			COMMAND install_name_tool -add_rpath ${RPATH} ${LIB} 2>/dev/null || true
			COMMENT "Adding rpath ${RPATH} to ${LIBNAME_BASE}")
		if(_ALR_AFTER)
			add_dependencies("${LIBNAME}_${_RPATH_SANITIZED}_add_rpath" ${_ALR_AFTER})
		endif()
		if(DEFINED _LAST_INSTALL_NAME_OP_${LIBNAME})
			add_dependencies("${LIBNAME}_${_RPATH_SANITIZED}_add_rpath" ${_LAST_INSTALL_NAME_OP_${LIBNAME}})
		endif()
		set("_LAST_INSTALL_NAME_OP_${LIBNAME}" "${LIBNAME}_${_RPATH_SANITIZED}_add_rpath")
	endmacro()

	macro(reset_library_dependent_name LIB OLD_PATH PATH)
		cmake_parse_arguments("_RDN" "" "" "AFTER" ${ARGN})
		get_filename_component(LIBNAME ${LIB} NAME)
		get_library_base_name("${LIBNAME}" LIBNAME_BASE)
		set(OLD_PATH_NAME "${OLD_PATH}")
		string(REPLACE "/" "_" OLD_PATH_NAME "${OLD_PATH_NAME}")
		string(REPLACE ":" "_" OLD_PATH_NAME "${OLD_PATH_NAME}")
		string(REPLACE "@" "_" OLD_PATH_NAME "${OLD_PATH_NAME}")
		add_custom_target("${LIBNAME}_${OLD_PATH_NAME}_reset_dependent_name"
			COMMAND install_name_tool -change ${OLD_PATH} ${PATH} ${LIB}
			COMMENT "Resetting Dependent Name ${OLD_PATH} -> ${PATH} in ${LIBNAME_BASE}")
		if(_RDN_AFTER)
			add_dependencies("${LIBNAME}_${OLD_PATH_NAME}_reset_dependent_name" ${_RDN_AFTER})
		endif()
		# Chain: serialize with any previous operation on this same library.
		if(DEFINED _LAST_INSTALL_NAME_OP_${LIBNAME})
			add_dependencies("${LIBNAME}_${OLD_PATH_NAME}_reset_dependent_name" ${_LAST_INSTALL_NAME_OP_${LIBNAME}})
		endif()
		set("_LAST_INSTALL_NAME_OP_${LIBNAME}" "${LIBNAME}_${OLD_PATH_NAME}_reset_dependent_name")
	endmacro()
	
	set(APP_BUNDLE_OUTPUT_DIR "${CMAKE_BINARY_DIR}/Apps")
	set(FRAMEWORK_OUTPUT_DIR "${CMAKE_BINARY_DIR}/Frameworks")
	
	set(UNSIGNED_TARGET_SUFFIX __unsigned)
endif()



if(XCODE)
    set(CMAKE_XCODE_ATTRIBUTE_CODE_SIGNING_IDENTITY ${CODE_SIGNATURE})
else()

    macro(code_sign_bundle _NAME IS_APP VERSION OUTPUT_DIR EMBED_LIBS)
        # Any extra args are real bundle-content files (embedded dylibs,
        # frameworks, resources) the signature must track. The __unsigned
        # aggregator is a phony target, so a custom command cannot see
        # timestamp changes through it — without these explicit file deps the
        # seal goes stale whenever a lib/framework re-embeds without the exe
        # relinking.
        set(_EXTRA_SIGN_DEPS ${ARGN})
        if(TARGET ${_NAME})
            if(${IS_APP})
                set(_OUT "${APP_BUNDLE_OUTPUT_DIR}/${_NAME}.app/Contents/_CodeSignature")
				set(_CODE "${APP_BUNDLE_OUTPUT_DIR}/${_NAME}.app")
            else()
                set(_OUT "${FRAMEWORK_OUTPUT_DIR}/${_NAME}.framework/Versions/${VERSION}/_CodeSignature")
				set(_CODE "${FRAMEWORK_OUTPUT_DIR}/${_NAME}.framework/Versions/${VERSION}")
            endif()
            
            # Track signing with a stamp FILE, not the _CodeSignature dir.
            # Ninja can't reliably up-to-date-check a directory output, so a
            # dir-output codesign re-runs every build; because codesign --force
            # rewrites the (framework) binary's signature in place, that churns
            # the binary's mtime and cascades into needless relinks + codesign
            # races (stray .cstemp files, stale seals). A stamp file re-runs
            # codesign only when a real input (exe / embedded artifact) changes.
            set(_STAMP_DIR "${CMAKE_BINARY_DIR}/CMakeFiles/codesign-stamps")
            file(MAKE_DIRECTORY "${_STAMP_DIR}")
            if(${IS_APP})
                set(_STAMP "${_STAMP_DIR}/${_NAME}.app.codesign.stamp")
            else()
                set(_STAMP "${_STAMP_DIR}/${_NAME}.framework.codesign.stamp")
            endif()

            if(${IS_APP})
                add_custom_command(OUTPUT "${_STAMP}"
                COMMAND ${PYEXEC} ${CODESIGN_SCRIPT}
                --sig ${CODE_SIGNATURE} --code ${_CODE}
                --strip-build-rpaths ${CMAKE_BINARY_DIR}
                COMMAND ${CMAKE_COMMAND} -E touch "${_STAMP}"
                DEPENDS "${_NAME}${UNSIGNED_TARGET_SUFFIX};${_NAME}" ${_EXTRA_SIGN_DEPS}
				COMMENT "Code Signing App Bundle ${_NAME}.app")
            else()
               
				if(EMBED_LIBS)
	                add_custom_command(OUTPUT "${_STAMP}"
	                COMMAND ${PYEXEC} ${CODESIGN_SCRIPT} --sig ${CODE_SIGNATURE} 
	                --code ${_CODE} 
	                --framework
	                -F "${FRAMEWORK_OUTPUT_DIR}/${_NAME}.framework"
	                --name ${_NAME}
	                --current_version ${VERSION}
					--symlink-other-dirs Libraries
	                COMMAND ${CMAKE_COMMAND} -E touch "${_STAMP}"
	                DEPENDS "${_NAME}${UNSIGNED_TARGET_SUFFIX};${_NAME}"
					COMMENT "Code Signing Framework Bundle ${_NAME}.framework"
	                )
				else()
	                add_custom_command(OUTPUT "${_STAMP}"
	                COMMAND ${PYEXEC} ${CODESIGN_SCRIPT} --sig ${CODE_SIGNATURE} 
	                --code ${_CODE} 
	                --framework
	                -F "${FRAMEWORK_OUTPUT_DIR}/${_NAME}.framework"
	                --name ${_NAME}
	                --current_version ${VERSION}
	                COMMAND ${CMAKE_COMMAND} -E touch "${_STAMP}"
	                DEPENDS "${_NAME}${UNSIGNED_TARGET_SUFFIX};${_NAME}"
					COMMENT "Code Signing Framework Bundle ${_NAME}.framework"
	                )
				endif()
            endif()
            add_custom_target("${_NAME}__codesign" DEPENDS "${_STAMP}")
        endif()
    endmacro(code_sign_bundle)  
    
endif()

# Resolve an EMBEDDED_LIBS entry. Accepts either a CMake target name (whose
# on-disk shared-lib filename is computed from TYPE/OUTPUT_NAME + the platform
# shared-lib suffix) or a path the caller already has. Writes three outputs in
# the parent scope: src_var = source argument for `cmake -E copy` (a path or
# $<TARGET_FILE:tgt> genex), name_var = basename of the staged copy,
# dep_var = what to put in DEPENDS (target name or path).
#
# Filename has to be computed at configure time because CMake's add_custom_command
# OUTPUT slot did not learn generator-expression support until 3.20 and this
# project's floor is 3.13.
function(_omega_resolve_embedded_lib lib src_var name_var dep_var)
    if(TARGET ${lib})
        get_target_property(_type ${lib} TYPE)
        get_target_property(_oname ${lib} OUTPUT_NAME)
        if(NOT _oname)
            set(_oname ${lib})
        endif()
        if(_type STREQUAL "MODULE_LIBRARY")
            set(_pfx "${CMAKE_SHARED_MODULE_PREFIX}")
            set(_sfx "${CMAKE_SHARED_MODULE_SUFFIX}")
        else()
            set(_pfx "${CMAKE_SHARED_LIBRARY_PREFIX}")
            set(_sfx "${CMAKE_SHARED_LIBRARY_SUFFIX}")
        endif()
        set(${src_var}  "$<TARGET_FILE:${lib}>" PARENT_SCOPE)
        set(${name_var} "${_pfx}${_oname}${_sfx}" PARENT_SCOPE)
        set(${dep_var}  ${lib} PARENT_SCOPE)
    else()
        get_filename_component(_n ${lib} NAME)
        set(${src_var}  "${lib}" PARENT_SCOPE)
        set(${name_var} "${_n}" PARENT_SCOPE)
        set(${dep_var}  "${lib}" PARENT_SCOPE)
    endif()
endfunction()

function(add_framework_bundle)
    cmake_parse_arguments("_ARG" "" "NAME;PLIST;VERSION" "SOURCES;RESOURCES;DEPS;LIBS;FRAMEWORKS;EMBEDDED_FRAMEWORKS;EMBEDDED_LIBS" ${ARGN})
	
	set(_NAME ${_ARG_NAME})
	
	
    add_library(${_NAME} SHARED ${_ARG_SOURCES})
	
	# get_target_property(_PREFIX ${_NAME} SUFFIX)
		#
	# message("${_PREFIX}")
	message("Framework Output Dir:${FRAMEWORK_OUTPUT_DIR}")
	file(MAKE_DIRECTORY ${FRAMEWORK_OUTPUT_DIR}/${_ARG_NAME}.framework)
	
    set_target_properties(${_ARG_NAME}
    PROPERTIES
	SUFFIX ""
	PREFIX ""
	MACOSX_RPATH TRUE
    LIBRARY_OUTPUT_DIRECTORY "${FRAMEWORK_OUTPUT_DIR}/${_ARG_NAME}.framework/Versions/${_ARG_VERSION}"
    # Stamp the framework's install_name at link time so downstream consumers
    # (e.g. libKREATE.dylib) record `@rpath/Foo.framework/Versions/X/Foo` in
    # their LC_LOAD_DYLIB. Without this, CMake defaults the install_name to
    # `@rpath/Foo` (MACOSX_RPATH + OUTPUT_NAME), and the codesign script's
    # post-hoc `install_name_tool -id` runs too late — consumers have already
    # been linked. INSTALL_NAME_DIR applies at install time; the BUILD_WITH_
    # variant pulls it into the build-tree binary too.
    INSTALL_NAME_DIR "@rpath/${_ARG_NAME}.framework/Versions/${_ARG_VERSION}"
    BUILD_WITH_INSTALL_NAME_DIR TRUE
    )

    # if(TARGET ${_NAME})
    #     message("FRAMEWORK:${_NAME}")
    # endif()
	set(UNSIGNED_TARGET "${_NAME}${UNSIGNED_TARGET_SUFFIX}")
    add_custom_target(${UNSIGNED_TARGET})
	add_dependencies(${UNSIGNED_TARGET} ${_ARG_NAME})
	
	set( _ARG_RESOURCES ${_ARG_RESOURCES} ${_ARG_PLIST})
	# message("RES: ${_ARG_RESOURCES}")
	
	set(ALL_RES_FINAL )
	
	file(MAKE_DIRECTORY "${FRAMEWORK_OUTPUT_DIR}/${_ARG_NAME}.framework/Versions/${_ARG_VERSION}/Resources")
	
    foreach(r ${_ARG_RESOURCES})
		get_filename_component(RES_NAME ${r} NAME)
		set(RES_OUTPUT "${FRAMEWORK_OUTPUT_DIR}/${_NAME}.framework/Versions/${_ARG_VERSION}/Resources/${RES_NAME}")
		list(APPEND ALL_RES_FINAL ${RES_OUTPUT})
        add_custom_command(
		OUTPUT ${RES_OUTPUT}
		COMMAND ${CMAKE_COMMAND} -E copy ${r}  "${FRAMEWORK_OUTPUT_DIR}/${_NAME}.framework/Versions/${_ARG_VERSION}/Resources/${RES_NAME}"
		DEPENDS ${r}
		COMMENT "Copying Bundle Resource ${RES_NAME}"
		)
    endforeach()
	
    add_custom_target("${_NAME}__res" DEPENDS ${ALL_RES_FINAL})
    add_dependencies(${UNSIGNED_TARGET} "${_NAME}__res")

    # if(${_ARG_EMBEDDED_HEADERS})
    #     set_target_properties()
    # set_source_files_properties(${_ARG_RESOURCES} MACOSX_PACKAGE_LOCATION "Resources")
	
	add_custom_target(${_NAME}.framework)

    if(XCODE)
		if(_ARG_EMBEDDED_FRAMEWORKS)
        	set_target_properties(${_NAME} PROPERTIES XCODE_EMBED_FRAMEWORKS ${_ARG_EMBEDDED_FRAMEWORKS})
		endif()
    else()
        set(__outputted_frameworks)
        foreach(f ${_ARG_EMBEDDED_FRAMEWORKS})
            set(__outputted_frameworks ${__outputted_frameworks} "${CMAKE_BINARY_DIR}/Frameworks/${_NAME}.framework/Versions/${_ARG_VERSION}/Frameworks/${f}.framework")
            add_dependencies(${_NAME} ${f})
            add_custom_command(
                OUTPUT "${FRAMEWORK_OUTPUT_DIR}/${_NAME}.framework/Versions/${_ARG_VERSION}/Frameworks/${f}.framework"
                COMMAND ${CMAKE_COMMAND} -E rm -rf "${FRAMEWORK_OUTPUT_DIR}/${_NAME}.framework/Versions/${_ARG_VERSION}/Frameworks/${f}.framework"
                COMMAND cp -R  "${FRAMEWORK_OUTPUT_DIR}/${f}.framework"  "${FRAMEWORK_OUTPUT_DIR}/${_NAME}.framework/Versions/${_ARG_VERSION}/Frameworks/${f}.framework"
                DEPENDS ${f}
				COMMENT "Embedding Framework ${f} in Framework Bundle ${_NAME}")
        endforeach()
        add_custom_target("${_NAME}__framework_embed" DEPENDS ${__outputted_frameworks})
        add_dependencies(${UNSIGNED_TARGET} "${_NAME}__framework_embed")
		
		set(EMBED_LIBS FALSE)
		
		if(_ARG_EMBEDDED_LIBS)
			set(EMBED_LIBS TRUE)
			set(_EMBED_DEST "${FRAMEWORK_OUTPUT_DIR}/${_NAME}.framework/Versions/${_ARG_VERSION}/Libraries")
			file(MAKE_DIRECTORY ${_EMBED_DEST})

			foreach(l ${_ARG_EMBEDDED_LIBS})
				_omega_resolve_embedded_lib(${l} _SRC _LIBNAME _DEP)
				set(_OUT "${_EMBED_DEST}/${_LIBNAME}")
				set(__outputted_libraries ${__outputted_libraries} "${_OUT}")
				add_custom_command(
						OUTPUT "${_OUT}"
						COMMAND ${CMAKE_COMMAND} -E copy "${_SRC}" "${_OUT}"
						DEPENDS ${_DEP}
						COMMENT "Embedding Library ${_LIBNAME} in Framework Bundle ${_NAME}")
			endforeach()
			add_custom_target("${_NAME}__lib_embed" DEPENDS ${__outputted_libraries})
			add_dependencies(${UNSIGNED_TARGET} "${_NAME}__lib_embed")
		endif()
    
        code_sign_bundle(${_NAME} FALSE ${_ARG_VERSION} "${FRAMEWORK_OUTPUT_DIR}/${_NAME}.framework/Versions/${_ARG_VERSION}" ${EMBED_LIBS})
		add_dependencies(${_NAME}.framework "${_NAME}__codesign")
    endif()
   
    
    foreach(_dep ${_ARG_DEPS})
        add_dependencies(${_NAME} ${_dep})
    endforeach()

    target_link_libraries(${_NAME} PRIVATE ${_ARG_LIBS} ${_ARG_FRAMEWORKS} ${_ARG_EMBEDDED_FRAMEWORKS})
    
endfunction()

function(add_app_bundle)
    cmake_parse_arguments("_ARG" "" "NAME;PLIST" "SOURCES;RESOURCES;DEPS;LIBS;EMBEDDED_FRAMEWORKS;EMBEDDED_LIBS" ${ARGN})
		
	message("KEYWORDS_MISSING_VALUES:${_ARG_KEYWORDS_MISSING_VALUES}")
	
	
    message("EMBEDDED_FRAMEWORKS:${_ARG_EMBEDDED_FRAMEWORKS}")

	set(_NAME ${_ARG_NAME})

    file(MAKE_DIRECTORY "${APP_BUNDLE_OUTPUT_DIR}/${_ARG_NAME}.app/Contents/MacOS")
    # Stage the bundle plist as Contents/Info.plist (NOT the source filename).
    # A caller that names it <App>_Info.plist (e.g. add_kreate_game) would
    # otherwise leave a stray, unsigned *_Info.plist in Contents/ that codesign
    # rejects ("not signed at all" subcomponent). Build-time (not configure-time
    # file(COPY)) so it is recreated on every build and tracked by the signature.
    set(_INFO_PLIST_OUT "${APP_BUNDLE_OUTPUT_DIR}/${_ARG_NAME}.app/Contents/Info.plist")
    add_custom_command(
        OUTPUT "${_INFO_PLIST_OUT}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${APP_BUNDLE_OUTPUT_DIR}/${_ARG_NAME}.app/Contents"
        COMMAND ${CMAKE_COMMAND} -E copy "${_ARG_PLIST}" "${_INFO_PLIST_OUT}"
        DEPENDS "${_ARG_PLIST}"
        COMMENT "Staging Info.plist for ${_ARG_NAME}.app")
    add_custom_target("${_NAME}__plist" DEPENDS "${_INFO_PLIST_OUT}")

    add_executable("${_ARG_NAME}" ${_ARG_SOURCES})
    set_target_properties("${_ARG_NAME}"
    PROPERTIES
    RUNTIME_OUTPUT_NAME ${_ARG_NAME}
			MACOS_RPATH TRUE
			SUFFIX ""
			PREFIX ""
			# Link the in-build app exe with the bundle-relative INSTALL_RPATH
			# below (@executable_path/..) instead of CMake's automatic build-tree
			# library dirs, so no absolute build/lib rpath is baked in. Everything
			# the app loads is embedded in the .app.
			BUILD_WITH_INSTALL_RPATH TRUE
    RUNTIME_OUTPUT_DIRECTORY "${APP_BUNDLE_OUTPUT_DIR}/${_ARG_NAME}.app/Contents/MacOS")

	# Bundle-relative rpaths for embedded artifacts. Without these, the
	# binary loads via @rpath but can't find the frameworks/libs that
	# add_custom_command staged into Contents/Frameworks and Contents/Libraries.
	# @executable_path resolves to .../Contents/MacOS at runtime.
	set_property(TARGET ${_ARG_NAME} APPEND PROPERTY BUILD_RPATH
		"@executable_path/../Frameworks"
		"@executable_path/../Libraries")
	set_property(TARGET ${_ARG_NAME} APPEND PROPERTY INSTALL_RPATH
		"@executable_path/../Frameworks"
		"@executable_path/../Libraries")

	set(UNSIGNED_TARGET "${_NAME}${UNSIGNED_TARGET_SUFFIX}")
	add_custom_target(${UNSIGNED_TARGET})
	add_dependencies(${UNSIGNED_TARGET} ${_ARG_NAME})
	add_dependencies(${UNSIGNED_TARGET} "${_NAME}__plist")

	set(ALL_RES_FINAL )

	file(MAKE_DIRECTORY "${APP_BUNDLE_OUTPUT_DIR}/${_ARG_NAME}.app/Contents/Resources")

	foreach(r ${_ARG_RESOURCES})
		get_filename_component(RES_NAME ${r} NAME)
		set(RES_OUTPUT "${APP_BUNDLE_OUTPUT_DIR}/${_ARG_NAME}.app/Contents/Resources/${RES_NAME}")
		list(APPEND ALL_RES_FINAL ${RES_OUTPUT})
		add_custom_command(
				OUTPUT ${RES_OUTPUT}
				COMMAND ${CMAKE_COMMAND} -E copy ${r}  "${APP_BUNDLE_OUTPUT_DIR}/${_ARG_NAME}.app/Contents/Resources/${RES_NAME}"
				DEPENDS ${r}
		)
	endforeach()

		add_custom_target("${_NAME}__res" DEPENDS ${ALL_RES_FINAL})
		add_dependencies(${UNSIGNED_TARGET} "${_NAME}__res")

	# if(${_ARG_EMBEDDED_HEADERS})
	#     set_target_properties()
	# set_source_files_properties(${_ARG_RESOURCES} MACOSX_PACKAGE_LOCATION "Resources")

	# ALL so a plain `cmake --build build` signs the bundle. Without it the
	# exe + embeds (which ARE in `all`) update but the codesign target is
	# never reached, leaving every app with a stale/missing signature until
	# someone builds the `<name>.app` target explicitly.
	add_custom_target(${_NAME}.app ALL)

	if(XCODE)
		if(_ARG_EMBEDDED_FRAMEWORKS)
			set_target_properties(${_NAME} PROPERTIES XCODE_EMBED_FRAMEWORKS "${_ARG_EMBEDDED_FRAMEWORKS}")
		endif()
	else()
		set(__outputted_frameworks)
		if(_ARG_EMBEDDED_FRAMEWORKS)
			file(MAKE_DIRECTORY ${APP_BUNDLE_OUTPUT_DIR}/${_NAME}.app/Contents/Frameworks)
		endif()
		foreach(f ${_ARG_EMBEDDED_FRAMEWORKS})
			set(__outputted_frameworks ${__outputted_frameworks} "${APP_BUNDLE_OUTPUT_DIR}/${_NAME}.app/Contents/Frameworks/${f}.framework")
			# The framework's Versions/Current + top-level symlinks (and its
			# signature) are produced by ${f}__codesign, NOT by building ${f}
			# itself. cp -R must wait for that step, otherwise it can copy the
			# framework before the symlinks exist and embed a broken bundle.
			set(_FW_EMBED_DEPS ${f})
			if(TARGET ${f}__codesign)
				list(APPEND _FW_EMBED_DEPS ${f}__codesign)
			endif()
			add_dependencies(${_NAME} ${_FW_EMBED_DEPS})
			add_custom_command(
					OUTPUT "${APP_BUNDLE_OUTPUT_DIR}/${_NAME}.app/Contents/Frameworks/${f}.framework"
					COMMAND ${CMAKE_COMMAND} -E rm -rf "${APP_BUNDLE_OUTPUT_DIR}/${_NAME}.app/Contents/Frameworks/${f}.framework"
					COMMAND cp -R "${FRAMEWORK_OUTPUT_DIR}/${f}.framework"  "${APP_BUNDLE_OUTPUT_DIR}/${_NAME}.app/Contents/Frameworks/${f}.framework"
					DEPENDS ${_FW_EMBED_DEPS}
					COMMENT "Embedding Framework ${f} in App Bundle ${_NAME}")
		endforeach()
			add_custom_target("${_NAME}__framework_embed" DEPENDS ${__outputted_frameworks})
			add_dependencies(${UNSIGNED_TARGET} "${_NAME}__framework_embed")
		
		set(EMBED_LIBS FALSE)
		
		if(_ARG_EMBEDDED_LIBS)
			set(EMBED_LIBS TRUE)
			set(_EMBED_DEST "${APP_BUNDLE_OUTPUT_DIR}/${_NAME}.app/Contents/Libraries")
			file(MAKE_DIRECTORY ${_EMBED_DEST})

			foreach(l ${_ARG_EMBEDDED_LIBS})
				_omega_resolve_embedded_lib(${l} _SRC _LIBNAME _DEP)
				set(_OUT "${_EMBED_DEST}/${_LIBNAME}")
				set(__outputted_libraries ${__outputted_libraries} "${_OUT}")
				add_custom_command(
						OUTPUT "${_OUT}"
						COMMAND ${CMAKE_COMMAND} -E copy "${_SRC}" "${_OUT}"
						DEPENDS ${_DEP}
						COMMENT "Embedding Library ${_LIBNAME} in App Bundle ${_NAME}")
			endforeach()
			add_custom_target("${_NAME}__lib_embed" DEPENDS ${__outputted_libraries})
			add_dependencies(${UNSIGNED_TARGET} "${_NAME}__lib_embed")
		endif()

			# Pass the real bundle-content files so the signature re-runs
			# whenever a resource, embedded framework, or embedded lib changes
			# — not only when the exe relinks. Without this a full parallel
			# build re-embeds artifacts but leaves the seal stale.
			set(_BUNDLE_SIGN_DEPS ${_INFO_PLIST_OUT} ${ALL_RES_FINAL} ${__outputted_frameworks} ${__outputted_libraries})
			code_sign_bundle(${_NAME} TRUE "VERSION" "${APP_BUNDLE_OUTPUT_DIR}/${_NAME}.app/Contents" ${EMBED_LIBS} ${_BUNDLE_SIGN_DEPS})
			add_dependencies(${_NAME}.app "${_NAME}__codesign")

			# Ensure a plain `${_NAME}` build also stages runtime bundle artifacts.
			add_dependencies(${_NAME} "${_NAME}__res")
			add_dependencies(${_NAME} "${_NAME}__framework_embed")
			if(TARGET "${_NAME}__lib_embed")
				add_dependencies(${_NAME} "${_NAME}__lib_embed")
			endif()
		endif()


	foreach(_dep ${_ARG_DEPS})
		add_dependencies(${_NAME} ${_dep})
	endforeach()

	target_link_libraries(${_NAME} PRIVATE ${_ARG_LIBS} ${_ARG_FRAMEWORKS} ${_ARG_EMBEDDED_FRAMEWORKS})
endfunction()

function(target_link_system_frameworks _NAME)
	set(FRAMEWORKS_TO_LINK ${ARGN})
	set(FRAMEWORKS_FLAGS)
	foreach(f ${FRAMEWORKS_TO_LINK})
		set(FRAMEWORKS_FLAGS "${FRAMEWORKS_FLAGS} -framework ${f}")
	endforeach()
	get_target_property(LINK_FLAGS_PRIOR ${_NAME} LINK_FLAGS)
	if(${LINK_FLAGS_PRIOR} STREQUAL "LINK_FLAGS_PRIOR-NOTFOUND")
		set_target_properties(${_NAME} PROPERTIES LINK_FLAGS ${FRAMEWORKS_FLAGS})
	else()
		set_target_properties(${_NAME} PROPERTIES LINK_FLAGS "${LINK_FLAGS_PRIOR} ${FRAMEWORKS_FLAGS}")
	endif()
	
endfunction()

function(target_link_frameworks _NAME)
	set(FRAMEWORKS_TO_LINK ${ARGN})
	set(FRAMEWORKS_FLAGS "-F${FRAMEWORK_OUTPUT_DIR}")
	set(FRAMEWORK_INCLUDE_DIRS)
	foreach(f ${FRAMEWORKS_TO_LINK})
		get_target_property(INC_DIR ${f} INCLUDE_DIRECTORIES)
		# message("${f} Include Dir: ${INC_DIR}")
		set(FRAMEWORK_INCLUDE_DIRS "${FRAMEWORK_INCLUDE_DIRS};${INC_DIR}")
		set(FRAMEWORKS_FLAGS "${FRAMEWORKS_FLAGS} -framework ${f}")
	endforeach()
	get_target_property(LINK_FLAGS_PRIOR ${_NAME} LINK_FLAGS)
	
	if(${LINK_FLAGS_PRIOR} STREQUAL "LINK_FLAGS_PRIOR-NOTFOUND")
		set_target_properties(${_NAME} PROPERTIES LINK_FLAGS ${FRAMEWORKS_FLAGS})
	else()
		set_target_properties(${_NAME} PROPERTIES LINK_FLAGS "${LINK_FLAGS_PRIOR} ${FRAMEWORKS_FLAGS}")
	endif()
	
	target_include_directories(${_NAME} PRIVATE ${FRAMEWORK_INCLUDE_DIRS})
	
endfunction()



include(ExternalProject)

# Root for third-party dependency BUILD + INSTALL trees.
#
# By default each module builds its deps under its own build tree
# (${CMAKE_CURRENT_BINARY_DIR}/deps), so a fresh build directory rebuilds every
# dep from scratch. Point OMEGA_THIRD_PARTY_BUILD_DIR at a stable location and
# every build dir on this machine reuses the SAME already-built deps — build
# once, reuse for any number of build dirs (e.g. a Debug tree and a
# RelWithDebInfo tree side by side).
#
# Deps are always built Release regardless of the parent CMAKE_BUILD_TYPE (see
# add_third_party's hardcoded -DCMAKE_BUILD_TYPE=Release), so config type is
# intentionally NOT part of the cache key — that is exactly why one cache safely
# serves every config. What DOES change a dep's ABI is the platform, the
# architecture and the compiler, so the shared root is keyed by those: two
# toolchains never collide on the same install path, and switching compilers
# builds a fresh keyed tree instead of silently reusing ABI-incompatible
# binaries (or fighting the compiler-drift cache wipe below). The key is
# computed lazily inside the function, not at include time, because
# CMAKE_CXX_COMPILER_ID/VERSION are only populated after project().
set(OMEGA_THIRD_PARTY_BUILD_DIR "" CACHE PATH
    "Stable root to build+install third-party deps once and reuse across build dirs; empty = per-build-tree (default)")

# omega_third_party_root(<out_var>) — set <out_var> to the third-party root for
# the CALLING directory. When OMEGA_THIRD_PARTY_BUILD_DIR is set, returns a
# toolchain-keyed subdir under it (shared across build dirs); otherwise returns
# the caller's ${CMAKE_CURRENT_BINARY_DIR}/deps (today's per-build-tree default).
function(omega_third_party_root out_var)
    if(OMEGA_THIRD_PARTY_BUILD_DIR)
        string(MAKE_C_IDENTIFIER
            "${CMAKE_SYSTEM_NAME}_${CMAKE_SYSTEM_PROCESSOR}_${CMAKE_CXX_COMPILER_ID}_${CMAKE_CXX_COMPILER_VERSION}"
            _key)
        set(${out_var} "${OMEGA_THIRD_PARTY_BUILD_DIR}/${_key}" PARENT_SCOPE)
    else()
        set(${out_var} "${CMAKE_CURRENT_BINARY_DIR}/deps" PARENT_SCOPE)
    endif()
endfunction()

function(add_third_party)

    cmake_parse_arguments(
        "_ARG" "CUSTOM_PROJECT" "NAME;SOURCE_DIR;BINARY_DIR;INSTALL_DIR"
        "DEPS;CMAKE_BUILD_ARGS;CONF;BUILD;INSTALL;EXPORT_STATIC_LIBS;EXPORT_SHARED_LIBS;EXPORT_INCLUDE_DIRS"
        ${ARGN})

    # Pre-create exported include directories so CMake's
    # INTERFACE_INCLUDE_DIRECTORIES validation passes at configure time.
    set(_EXPORT_INC_DIRS)
    if(_ARG_EXPORT_INCLUDE_DIRS)
        foreach(_inc ${_ARG_EXPORT_INCLUDE_DIRS})
            set(_abs_inc "${_ARG_INSTALL_DIR}/${_inc}")
            file(MAKE_DIRECTORY "${_abs_inc}")
            list(APPEND _EXPORT_INC_DIRS "${_abs_inc}")
        endforeach()
    endif()

    # Collect library file paths as BUILD_BYPRODUCTS so Ninja knows
    # which ExternalProject step produces each file.
    set(_BYPRODUCTS)
    foreach(_export ${_ARG_EXPORT_STATIC_LIBS} ${_ARG_EXPORT_SHARED_LIBS})
        string(REPLACE ":" ";" _bp "${_export}")
        list(LENGTH _bp _bp_n)
        if(_bp_n EQUAL 3)
            list(GET _bp 1 _bp_unix)
            list(GET _bp 2 _bp_win)
        elseif(_bp_n EQUAL 2)
            list(GET _bp 1 _bp_unix)
            set(_bp_win "${_bp_unix}")
        else()
            continue()
        endif()
        if(WIN32)
            list(APPEND _BYPRODUCTS "${_ARG_INSTALL_DIR}/${_bp_win}")
        else()
            list(APPEND _BYPRODUCTS "${_ARG_INSTALL_DIR}/${_bp_unix}")
        endif()
    endforeach()

    if(NOT ${_ARG_CUSTOM_PROJECT})
        message("Adding ThirdParty: CMAKE PROJECT:${_ARG_NAME}")
        # If a previous configure cached a different compiler than the parent
        # is currently using, the child's CMakeCache.txt will pin that stale
        # value and silently override the -DCMAKE_C_COMPILER we pass below.
        # This bites on Windows where a child can pick up MSVC's
        # version-stamped cl.exe path (e.g. .../MSVC/14.50.35717/.../cl.exe)
        # that ceases to exist after MSVC servicing. Detect drift against the
        # parent and wipe the child cache + CMakeFiles so the new configure
        # honors the forwarded compiler.
        set(_OG_CHILD_CACHE "${_ARG_BINARY_DIR}/CMakeCache.txt")
        if(EXISTS "${_OG_CHILD_CACHE}")
            set(_OG_CHILD_STALE FALSE)
            foreach(_OG_VAR CMAKE_C_COMPILER CMAKE_CXX_COMPILER)
                file(STRINGS "${_OG_CHILD_CACHE}" _OG_LINES
                    REGEX "^${_OG_VAR}:[A-Z]+=")
                if(_OG_LINES)
                    list(GET _OG_LINES 0 _OG_LINE)
                    string(REGEX REPLACE "^${_OG_VAR}:[A-Z]+=" "" _OG_CACHED "${_OG_LINE}")
                    if(NOT "${_OG_CACHED}" STREQUAL "${${_OG_VAR}}")
                        message(STATUS
                            "add_third_party(${_ARG_NAME}): cached ${_OG_VAR}='${_OG_CACHED}' "
                            "differs from parent '${${_OG_VAR}}'; invalidating child cache")
                        set(_OG_CHILD_STALE TRUE)
                    endif()
                endif()
            endforeach()
            if(_OG_CHILD_STALE)
                file(REMOVE "${_OG_CHILD_CACHE}")
                file(REMOVE_RECURSE "${_ARG_BINARY_DIR}/CMakeFiles")
            endif()
        endif()
        # Force Release for every third-party build. -DCMAKE_BUILD_TYPE pins
        # single-config generators (Ninja, Make); --config Release pins
        # multi-config generators (Visual Studio, Xcode, Ninja Multi-Config),
        # which silently ignore CMAKE_BUILD_TYPE and default to Debug otherwise.
        # Passing --config on a single-config generator is a no-op, so this
        # works uniformly across every generator we support.
        # CMAKE_C_COMPILER/CMAKE_CXX_COMPILER are forwarded explicitly so the
        # child locks to the parent's toolchain choice instead of auto-detecting
        # from PATH (which on Windows can silently fall through to MSVC's
        # cl.exe even when the parent is clang-cl).
        # CMAKE_POLICY_VERSION_MINIMUM=3.5 is CMake's official escape hatch for
        # CMake 4.x: it dropped compatibility with cmake_minimum_required(<3.5),
        # and every bundled third-party source whose top-level CMakeLists still
        # declares an older minimum (libjpeg-turbo is the obvious one, but the
        # trap is generic) refuses to configure without it. Harmless for deps
        # that already require >= 3.5.
        ExternalProject_Add(
            ${_ARG_NAME}
            SOURCE_DIR "${_ARG_SOURCE_DIR}"
            BINARY_DIR "${_ARG_BINARY_DIR}"
            INSTALL_DIR "${_ARG_INSTALL_DIR}"
            CONFIGURE_COMMAND ${CMAKE_COMMAND} -S ${_ARG_SOURCE_DIR} -G${CMAKE_GENERATOR} -B ${_ARG_BINARY_DIR} -DCMAKE_BUILD_TYPE=Release -DCMAKE_CONFIGURATION_TYPES=Release -DCMAKE_INSTALL_PREFIX=${_ARG_INSTALL_DIR} -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER} -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER} -DCMAKE_POLICY_VERSION_MINIMUM=3.5 ${_ARG_CMAKE_BUILD_ARGS}
            BUILD_COMMAND ${CMAKE_COMMAND} --build ${_ARG_BINARY_DIR} --config Release
            INSTALL_COMMAND ${CMAKE_COMMAND} --install ${_ARG_BINARY_DIR} --config Release
            BUILD_BYPRODUCTS ${_BYPRODUCTS}
            DEPENDS ${_ARG_DEPS}
        )
    else()
        message("Adding ThirdParty: CUSTOM PROJECT:${_ARG_NAME}")
        ExternalProject_Add(
            ${_ARG_NAME}
            SOURCE_DIR "${_ARG_SOURCE_DIR}"
            BINARY_DIR "${_ARG_BINARY_DIR}"
            INSTALL_DIR "${_ARG_INSTALL_DIR}"
            CONFIGURE_COMMAND "${_ARG_CONF}"
            BUILD_COMMAND "${_ARG_BUILD}"
            INSTALL_COMMAND "${_ARG_INSTALL}"
            BUILD_BYPRODUCTS ${_BYPRODUCTS}
            DEPENDS ${_ARG_DEPS}
        )
    endif()

    # Create IMPORTED STATIC library targets for each export entry.
    # Format: "target_name:unix_lib_path:win_lib_path"  (paths relative to INSTALL_DIR)
    #     or: "target_name:lib_path"                     (same path on all platforms)
    foreach(_export ${_ARG_EXPORT_STATIC_LIBS})
        string(REPLACE ":" ";" _parts "${_export}")
        list(LENGTH _parts _nparts)
        list(GET _parts 0 _tgt)

        if(_nparts EQUAL 3)
            list(GET _parts 1 _unix_rel)
            list(GET _parts 2 _win_rel)
        elseif(_nparts EQUAL 2)
            list(GET _parts 1 _unix_rel)
            set(_win_rel "${_unix_rel}")
        else()
            message(FATAL_ERROR "EXPORT_STATIC_LIBS: expected target:path or target:unix_path:win_path, got '${_export}'")
        endif()

        if(WIN32)
            set(_lib_location "${_ARG_INSTALL_DIR}/${_win_rel}")
        else()
            set(_lib_location "${_ARG_INSTALL_DIR}/${_unix_rel}")
        endif()

        add_library(${_tgt} STATIC IMPORTED GLOBAL)
        set_target_properties(${_tgt} PROPERTIES IMPORTED_LOCATION "${_lib_location}")
        add_dependencies(${_tgt} ${_ARG_NAME})

        if(_EXPORT_INC_DIRS)
            set_target_properties(${_tgt} PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${_EXPORT_INC_DIRS}")
        endif()
    endforeach()

    # Create IMPORTED SHARED library targets for each export entry.
    # Same format as EXPORT_STATIC_LIBS. On Windows the path is treated as
    # the import library (IMPORTED_IMPLIB); on Unix it is the shared library
    # itself (IMPORTED_LOCATION).
    foreach(_export ${_ARG_EXPORT_SHARED_LIBS})
        string(REPLACE ":" ";" _parts "${_export}")
        list(LENGTH _parts _nparts)
        list(GET _parts 0 _tgt)

        if(_nparts EQUAL 3)
            list(GET _parts 1 _unix_rel)
            list(GET _parts 2 _win_rel)
        elseif(_nparts EQUAL 2)
            list(GET _parts 1 _unix_rel)
            set(_win_rel "${_unix_rel}")
        else()
            message(FATAL_ERROR "EXPORT_SHARED_LIBS: expected target:path or target:unix_path:win_path, got '${_export}'")
        endif()

        add_library(${_tgt} SHARED IMPORTED GLOBAL)
        if(WIN32)
            set_target_properties(${_tgt} PROPERTIES IMPORTED_IMPLIB "${_ARG_INSTALL_DIR}/${_win_rel}")
        else()
            set_target_properties(${_tgt} PROPERTIES IMPORTED_LOCATION "${_ARG_INSTALL_DIR}/${_unix_rel}")
        endif()
        add_dependencies(${_tgt} ${_ARG_NAME})

        if(_EXPORT_INC_DIRS)
            set_target_properties(${_tgt} PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${_EXPORT_INC_DIRS}")
        endif()
    endforeach()

endfunction()




# Lazily configure the host-tools superbuild. Called by add_omega_graphics_tool
# the first time it runs in a cross-compile parent build. The superbuild
# reconfigures the source tree without our toolchain file (so it inherits the
# host's default compilers/RHI) and with -DOMEGA_HOST_TOOLS_ONLY=ON, which
# scopes the host build to the libraries and tools that omegaslc / autom /
# omega-wrapgen / ... need.
function(omega_init_host_tools)
	if(TARGET omega-host-tools)
		return()
	endif()
	include(ExternalProject)
	set(_HOST_TOOLS_DIR "${CMAKE_BINARY_DIR}/_host_tools")
	# Stash the path globally so add_omega_graphics_tool can resolve binaries.
	set(OMEGA_HOST_TOOLS_DIR "${_HOST_TOOLS_DIR}" CACHE INTERNAL "Host-tools superbuild binary dir")

	set(_HOST_CMAKE_ARGS
		"-DOMEGA_HOST_TOOLS_ONLY=ON"
		"-DCMAKE_BUILD_TYPE=Release")
	if(DEFINED CODE_SIGNATURE)
		list(APPEND _HOST_CMAKE_ARGS "-DCODE_SIGNATURE=${CODE_SIGNATURE}")
	elseif(CMAKE_HOST_APPLE)
		# Ad-hoc signature for local-host executables.
		list(APPEND _HOST_CMAKE_ARGS "-DCODE_SIGNATURE=-")
	endif()

	ExternalProject_Add(omega-host-tools
		SOURCE_DIR "${CMAKE_SOURCE_DIR}"
		BINARY_DIR "${_HOST_TOOLS_DIR}"
		INSTALL_COMMAND ""
		CMAKE_ARGS ${_HOST_CMAKE_ARGS}
		BUILD_ALWAYS TRUE
	)
endfunction()


# add_omega_graphics_tool — declares a developer tool executable.
#
# Tools always run on the developer's host machine (shader compilers, asset
# bundlers, codegen, etc.), so when the parent build is cross-compiling,
# we don't actually want to build the tool for the device. Instead we:
#
#   1. Generate a tiny shim source ("int main(void){return 0;}") and build a
#      real executable target from it for the cross target. This preserves
#      the full target API: callers can still `target_link_libraries`,
#      `set_target_properties`, `add_custom_command(TARGET ... POST_BUILD)`,
#      and resolve `$<TARGET_FILE:${_NAME}>` against this target.
#   2. Spin up a one-time host superbuild (via omega_init_host_tools) that
#      builds the same source tree on the host with -DOMEGA_HOST_TOOLS_ONLY=ON.
#   3. Add a POST_BUILD step that overwrites the shim binary with the host
#      superbuild's matching output.
#
# Net result: $<TARGET_FILE:${_NAME}> always points at a binary that runs on
# the developer's host, regardless of the cross-compile target. Subsequent
# `target_link_libraries` calls on the shim are accepted (they link into the
# unused shim, which is harmless) and `add_custom_command(TARGET ${_NAME}
# POST_BUILD ...)` still fires after the host binary is in place.
function(add_omega_graphics_tool _NAME)
	cmake_parse_arguments("_ARG" "" "" "LIBS;SOURCES" ${ARGN})

	if(CMAKE_CROSSCOMPILING AND NOT OMEGA_HOST_TOOLS_ONLY)
		omega_init_host_tools()

		set(_shim_dir "${CMAKE_BINARY_DIR}/_omega_tool_shims")
		set(_shim_src "${_shim_dir}/${_NAME}.c")
		file(MAKE_DIRECTORY "${_shim_dir}")
		if(NOT EXISTS "${_shim_src}")
			file(WRITE "${_shim_src}"
				"/* Auto-generated cross-compile shim for omega graphics tool '${_NAME}'.\n"
				" * The real binary is produced by the host-tools superbuild and copied\n"
				" * over this file by a POST_BUILD step in add_omega_graphics_tool. */\n"
				"int main(void) { return 0; }\n")
		endif()

		add_executable(${_NAME} "${_shim_src}")
		# Tools are plain CLI binaries — never wrap them in a .app/.framework
		# bundle on Apple cross-compile targets like iOS.
		set_target_properties(${_NAME} PROPERTIES
			RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
			MACOSX_BUNDLE FALSE)
		install(TARGETS ${_NAME} RUNTIME DESTINATION bin)
		add_dependencies(${_NAME} omega-host-tools)
		foreach(dep ${_ARG_LIBS})
			add_dependencies(${_NAME} ${dep})
		endforeach()

		if(CMAKE_HOST_WIN32)
			set(_host_path "${OMEGA_HOST_TOOLS_DIR}/bin/${_NAME}.exe")
		else()
			set(_host_path "${OMEGA_HOST_TOOLS_DIR}/bin/${_NAME}")
		endif()

		add_custom_command(TARGET ${_NAME} POST_BUILD
			COMMAND ${CMAKE_COMMAND} -E copy_if_different
				"${_host_path}" "$<TARGET_FILE:${_NAME}>"
			COMMENT "Installing host-built ${_NAME} over cross-compile shim"
			VERBATIM)
		return()
	endif()

	set(_SOURCES ${_ARG_SOURCES})
	add_executable(${_NAME} ${_SOURCES})
	set_target_properties(${_NAME} PROPERTIES
		RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
		MACOSX_BUNDLE FALSE)
	install(TARGETS ${_NAME} RUNTIME DESTINATION bin)
	foreach(dep ${_ARG_LIBS})
		add_dependencies(${_NAME} ${dep})
	endforeach()
	target_link_libraries(${_NAME} PRIVATE ${_ARG_LIBS})
endfunction()

function(add_omega_graphics_test _NAME)
	cmake_parse_arguments("_ARG" "" "" "LIBS;SOURCES" ${ARGN})
	set(_SOURCES ${_ARG_SOURCES})
	add_executable(${_NAME} ${_SOURCES})
	set_target_properties(${_NAME} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests)
	# NOT INSTALLED UNLIKE TOOLS
    foreach(dep ${_ARG_LIBS})
        add_dependencies(${_NAME} ${dep})
    endforeach()
    target_link_libraries(${_NAME} PRIVATE ${_ARG_LIBS})
endfunction()



function(add_omega_graphics_module _NAME)
	
	cmake_parse_arguments("_ARG" "STATIC;SHARED;FRAMEWORK;CUSTOM" "HEADER_DIR;INFO_PLIST;VERSION" "DEPENDS;SOURCES;EMBEDDED_LIBS" ${ARGN})
	
	message("-- Adding Module:${_NAME}")

	if(${_ARG_STATIC})
		add_library(${_NAME} STATIC ${_ARG_SOURCES})
		set_target_properties(${_NAME} PROPERTIES ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
		install(TARGETS ${_NAME} ARCHIVE DESTINATION lib)

		foreach(dep ${_ARG_DEPENDS})
			add_dependencies(${_NAME} ${dep})
		endforeach()
	elseif(${_ARG_CUSTOM})
	# Allows for custom installation locations as well suffixes and output directories during build
		add_library(${_NAME} MODULE ${_ARG_SOURCES})
		foreach(dep ${_ARG_DEPENDS})
			add_dependencies(${_NAME} ${dep})
		endforeach()
	else()

		if(APPLE AND ${_ARG_FRAMEWORK})
			set(EMBED_LIBS_ARGS)
			if(_ARG_EMBEDDED_LIBS)
				set(EMBED_LIBS_ARGS "EMBEDDED_LIBS;${_ARG_EMBEDDED_LIBS}")
			endif()
			if(_ARG_DEPENDS)
				add_framework_bundle(NAME ${_NAME} PLIST ${_ARG_INFO_PLIST} VERSION ${_ARG_VERSION} ${EMBED_LIBS_ARGS} DEPS ${_ARG_DEPENDS} SOURCES ${_ARG_SOURCES})
			else()
				add_framework_bundle(NAME ${_NAME} PLIST ${_ARG_INFO_PLIST} VERSION ${_ARG_VERSION} ${EMBED_LIBS_ARGS} SOURCES ${_ARG_SOURCES})
			endif()
			install(DIRECTORY ${FRAMEWORK_OUTPUT_DIR}/${_NAME}.framework DESTINATION lib)
		else()

			add_library(${_NAME} SHARED ${_ARG_SOURCES})
			if(WIN32)
				set_target_properties(${_NAME} PROPERTIES
				ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib
				RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

				install(TARGETS ${_NAME} RUNTIME DESTINATION bin ARCHIVE DESTINATION lib)
			else()
				set_target_properties(${_NAME} PROPERTIES LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
				install(TARGETS ${_NAME} LIBRARY DESTINATION lib)
			endif()

			# Handle EMBEDDED_LIBS for non-FRAMEWORK SHARED modules. The
			# semantic is: each entry is the runtime artifact path (.dll on
			# Win32, .dylib on macOS, .so on Linux). Each artifact is copied
			# next to ${_NAME}'s shared library output, and the appropriate
			# rpath token is added so the dynamic linker finds them at run
			# time without external configuration.
			#
			# Caller is responsible for passing platform-correct paths —
			# see e.g. ICU_RUNTIME in common/CMakeLists.txt.
			if(_ARG_EMBEDDED_LIBS)
				if(WIN32)
					# Win loader checks the binary's directory automatically;
					# no rpath equivalent needed. RUNTIME_OUTPUT_DIRECTORY is
					# ${CMAKE_BINARY_DIR}/bin per the block above.
					set(_OMEGA_EMBED_DEST "${CMAKE_BINARY_DIR}/bin")
				elseif(APPLE)
					set(_OMEGA_EMBED_DEST "${CMAKE_BINARY_DIR}/lib")
					set_property(TARGET ${_NAME} APPEND PROPERTY BUILD_RPATH   "@loader_path")
					set_property(TARGET ${_NAME} APPEND PROPERTY INSTALL_RPATH "@loader_path")
				else()
					set(_OMEGA_EMBED_DEST "${CMAKE_BINARY_DIR}/lib")
					set_property(TARGET ${_NAME} APPEND PROPERTY BUILD_RPATH   "$ORIGIN")
					set_property(TARGET ${_NAME} APPEND PROPERTY INSTALL_RPATH "$ORIGIN")
				endif()
				foreach(_OMEGA_EMBED_LIB IN LISTS _ARG_EMBEDDED_LIBS)
					get_filename_component(_OMEGA_EMBED_NAME "${_OMEGA_EMBED_LIB}" NAME)
					add_custom_command(TARGET ${_NAME} POST_BUILD
						COMMAND ${CMAKE_COMMAND} -E copy_if_different
							"${_OMEGA_EMBED_LIB}"
							"${_OMEGA_EMBED_DEST}/${_OMEGA_EMBED_NAME}"
						VERBATIM
						COMMENT "Embedding ${_OMEGA_EMBED_NAME} alongside ${_NAME}")
				endforeach()
			endif()

			foreach(dep ${_ARG_DEPENDS})
				add_dependencies(${_NAME} ${dep})
			endforeach()

		endif()

	endif()

	install(DIRECTORY ${_ARG_HEADER_DIR} DESTINATION "include")
    target_include_directories(${_NAME} PUBLIC ${_ARG_HEADER_DIR})



	

endfunction()

function(omega_graphics_project _NAME)
	if(${_NAME}_INCLUDE)
	else()
		message("Project ${_NAME} ${ARGN}")
		project(${_NAME} ${ARGN})
	endif()

endfunction()



function(omega_graphics_add_subdir _PROJECT_NAME _NAME)

	set(${_PROJECT_NAME}_INCLUDE TRUE)
	add_subdirectory(${_NAME})

endfunction()



# omega_stage_runtime_dlls(<consumer_target>)
#
# Windows-only. Adds a POST_BUILD step that copies every *.dll from
# ${CMAKE_BINARY_DIR}/bin into the consumer target's runtime output directory.
#
# The convention is that every Omega graphics module that owns a shared
# third-party dependency stages those DLLs into ${CMAKE_BINARY_DIR}/bin as
# part of its own build (alongside the module's own *.dll). App / test
# builders then call this helper to fan everything out into each binary's
# directory so the loader finds it at run time.
#
# Currently staged into bin by:
#   OmegaCommon  -> ICU (icuuc, icudata, icui18n) + OmegaCommon.dll itself
#   OmegaWTK     -> libxml2 + OmegaWTK.dll itself
#   OmegaGTE     -> OmegaGTE.dll itself
#   (modules)    -> their own *.dll via the SHARED branch's RUNTIME_OUTPUT_DIRECTORY
#
# No-op on non-Windows platforms (rpath / @loader_path handles colocation there).
function(omega_stage_runtime_dlls _NAME)
	if(NOT WIN32)
		return()
	endif()
	if(NOT TARGET ${_NAME})
		message(FATAL_ERROR "omega_stage_runtime_dlls: target '${_NAME}' does not exist")
	endif()
	add_custom_command(TARGET ${_NAME} POST_BUILD
		COMMAND ${CMAKE_COMMAND}
			-DSRC=${CMAKE_BINARY_DIR}/bin
			-DDST=$<TARGET_FILE_DIR:${_NAME}>
			-P ${OMEGA_COPY_DLLS_SCRIPT}
		VERBATIM
		COMMENT "Staging runtime DLLs into output dir of ${_NAME}")
endfunction()
