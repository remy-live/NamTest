cmake_minimum_required(VERSION 3.10)

project(NeuralAmpModelerLv2 VERSION 0.2.3)

set(CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
set(LIB_INSTALL_DIR ${CMAKE_INSTALL_PREFIX}/lib CACHE PATH "The library install dir (default: lib)")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED OFF)
set(CMAKE_CXX_EXTENSIONS OFF)

if (CMAKE_SYSTEM_NAME STREQUAL "Darwin")
	include_directories(SYSTEM /usr/local/include)
elseif (CMAKE_SYSTEM_NAME STREQUAL "Linux")
	link_libraries( "$<$<AND:$<CXX_COMPILER_ID:GNU>,$<VERSION_LESS:$<CXX_COMPILER_VERSION>,9.0>>:-lstdc++fs>" )
elseif (CMAKE_SYSTEM_NAME STREQUAL "Windows")
	add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN)
else()
	message(FATAL_ERROR "Unrecognized Platform!")
endif()

set(NAM_LV2_ID http://remy.local/nam-test)

add_subdirectory(src)


# Le bundle s'appelle nam_test.lv2, PAS neural_amp_modeler.lv2.
#
# Deux raisons, et la seconde coute cher :
#   - c'est le nom deja pose sur la machine, celui que la procedure
#     d'installation efface et remplace ;
#   - le NAM du magasin s'installe sous neural_amp_modeler.lv2. Deux dossiers
#     du meme nom, ou deux bundles qui declarent le meme greffon, et mod-ui ne
#     sait plus lequel prendre.
# Le fichier .so et les .ttl gardent leur nom DANS le bundle : manifest.ttl les
# designe par un chemin relatif, et c'est ainsi depuis le debut.
set(NAM_BUNDLE nam_test.lv2)

add_custom_target(copy_binaries ALL
	${CMAKE_COMMAND} -E copy "$<TARGET_FILE:neural_amp_modeler>" ${NAM_BUNDLE}/
	DEPENDS neural_amp_modeler
)

file(COPY resources/modgui.ttl DESTINATION ${NAM_BUNDLE})
file(COPY resources/modgui DESTINATION ${NAM_BUNDLE})

configure_file(resources/manifest.ttl.in ${NAM_BUNDLE}/manifest.ttl)
configure_file(resources/neural_amp_modeler.ttl.in ${NAM_BUNDLE}/neural_amp_modeler.ttl)

install (DIRECTORY ${CMAKE_BINARY_DIR}/${NAM_BUNDLE}
       DESTINATION ${LIB_INSTALL_DIR}/lv2
)

set(CPACK_GENERATOR "DEB")
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "oliphant@nostatic.org>")
set(CPACK_PACKAGE_VENDOR "Mike Oliphant")
set(CPACK_PACKAGE_DESCRIPTION "Neural Amp Modeler LV2 plugin implementation")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Neural Amp Modeler LV2 plugin")
set(CPACK_DEBIAN_PACKAGE_SECTION sound)
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
set(CPACK_DEBIAN_PACKAGE_CONTROL_STRICT_PERMISSION TRUE)
set(CPACK_PACKAGING_INSTALL_PREFIX /usr)
set(CPACK_PROJECT_NAME ${PROJECT_NAME})
set(CPACK_PROJECT_VERSION ${PROJECT_VERSION})
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
set (CPACK_STRIP_FILES ON)

include(CPack)
