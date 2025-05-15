# SPDX-License-Identifier: CC0-1.0
#
# SPDX-FileContributor: Antonio Niño Díaz, 2024

BLOCKSDS        ?= /opt/blocksds/core

# Target config
# BOARD can be "sd" or "lite"
BOARD ?= sd

ifeq ($(BOARD),lite)
  DEFINES ?= -DSUPERCARD_LITE
else ifeq ($(BOARD),sd)
  # No need for extra flags
else
  $(error No valid board specified in BOARD)
endif

# User config

NAME            := superfw-flasher-$(BOARD)
GAME_TITLE      := SuperFW flashing tool
GAME_SUBTITLE   := davidgf
GAME_AUTHOR	    ?= davidgf.net

include $(BLOCKSDS)/sys/default_makefiles/rom_arm9/Makefile

