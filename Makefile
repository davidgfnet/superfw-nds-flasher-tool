# SPDX-License-Identifier: CC0-1.0
#
# SPDX-FileContributor: Antonio Niño Díaz, 2024

BLOCKSDS        ?= /opt/blocksds/core

# User config

NAME            := superfw-flasher
GAME_TITLE      := SuperFW flashing tool
GAME_SUBTITLE   := SuperFW flashing tool

include $(BLOCKSDS)/sys/default_makefiles/rom_arm9/Makefile

