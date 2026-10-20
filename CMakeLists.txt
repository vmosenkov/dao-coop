cmake_minimum_required(VERSION 3.21)

project(dao_coop VERSION 0.0.1 LANGUAGES CXX)

# DAOLib is brought in as a submodule, the way a Skyrim plugin brings in
# CommonLibSSE (DAOLib's dev/PLUGINS.md). It gives us the `daolib` library
# and `add_daolib_plugin`; its own loader target is declared too, so a build
# of this tree also produces the dinput8.dll the plugin needs.
if(NOT TARGET DAOLib::daolib)
    add_subdirectory(extern/DAOLib EXCLUDE_FROM_ALL)
endif()

# The co-op plugin. One DLL, loaded by DAOLib from
# bin_ship\daolib\plugins\daocoop\daocoop.dll.
add_daolib_plugin(daocoop SOURCES src/Plugin.cpp src/Transport.cpp)
dao_apply_warnings(daocoop)
# Winsock for the loopback transport (milestone 2).
target_link_libraries(daocoop PRIVATE ws2_32)
