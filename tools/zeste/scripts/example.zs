# Zeste automation script example (see src/core/zeste_automation.h for every command).
# Run:  zeste.exe --headless --script tools\zeste\scripts\example.zs path\to\app.3dsx
@60   lcd both  C:/temp/zeste/boot.png        # raw LCD contents after one second
+10   press A 5                               # hold A for 5 frames
+30   touch 160 120 3                         # tap the middle of the bottom screen
+30   stick 0 156 30                          # circle pad up for half a second
+40   dump-vram C:/temp/zeste/vram.bin
+1    report C:/temp/zeste/report             # inaccuracy report so far
+10   quit 0
