cmake_minimum_required(VERSION 3.16)

project(obs-fsr1-filter VERSION 0.1.0 LANGUAGES C)

find_package(libobs REQUIRED)

add_library(${PROJECT_NAME} MODULE src/plugin.c)

target_link_libraries(${PROJECT_NAME} PRIVATE OBS::libobs m)

set_target_properties(
  ${PROJECT_NAME}
  PROPERTIES PREFIX ""
             C_STANDARD 17
             C_STANDARD_REQUIRED ON
             C_VISIBILITY_PRESET hidden)

target_compile_options(${PROJECT_NAME} PRIVATE -Wall -Wextra -Wno-unused-parameter)

# Installationspfade im "klassischen" OBS-Layout, das auch die Flatpak-
# Erweiterungen (/app/plugins/<Name>/...) nutzen:
#   <prefix>/lib/obs-plugins/obs-fsr1-filter.so
#   <prefix>/share/obs/obs-plugins/obs-fsr1-filter/{effects,locale}
# Bei Bedarf überschreiben, z. B. -DOBS_PLUGIN_DESTINATION=lib/x86_64-linux-gnu/obs-plugins
set(OBS_PLUGIN_DESTINATION
    "lib/obs-plugins"
    CACHE STRING "Zielordner der Plugin-Bibliothek (relativ zum Prefix)")
set(OBS_DATA_DESTINATION
    "share/obs/obs-plugins/${PROJECT_NAME}"
    CACHE STRING "Zielordner der Plugin-Daten (relativ zum Prefix)")

install(TARGETS ${PROJECT_NAME} LIBRARY DESTINATION "${OBS_PLUGIN_DESTINATION}")
install(DIRECTORY data/ DESTINATION "${OBS_DATA_DESTINATION}")
