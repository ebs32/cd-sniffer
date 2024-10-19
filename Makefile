#
# This is a project Makefile. It is assumed the directory this Makefile resides in is a
# project subdirectory.
#

PROJECT_NAME         := cd-sniffer
EXTRA_COMPONENT_DIRS := src/sender

include $(IDF_PATH)/make/project.mk
