@echo off

rem For use when manually building a driver, use the add and remove scripts to bring up and down a permanent virtual display.
rem Always run the remove script before installing a new driver build or bad things may happen.
rem
rem Note that virtualdisplay.exe needs to be run from a protected directory, I have just been using the Sunshine directory.

set "VIRTUALDISPLAY=C:\Program Files\Sunshine\virtualdisplay.exe"
set "VK_LAYER_JSON=%~dp0\build-driver\src\driver\VkLayer_sunshine_hdr.json"

"%VIRTUALDISPLAY%" broker install
"%VIRTUALDISPLAY%" broker start
"%VIRTUALDISPLAY%" permanent set --count 1 --width 3840 --height 2160 --refresh 240 --name "Sunshine 4K"
"%VIRTUALDISPLAY%" vulkan-layer install --json "%VK_LAYER_JSON%"
"%VIRTUALDISPLAY%" vulkan-layer status
"%VIRTUALDISPLAY%" status
