disable-two-finger-click.exe: disable-two-finger-click.c
	x86_64-w64-mingw32-gcc -o disable-two-finger-click -O99 disable-two-finger-click.c -lhid -mwindows 
