all: simple-playback.c mixer-recoder.c
	gcc simple-playback.c -o play
	gcc mixer-recoder.c -o mr

playback: simple-playback.c
	gcc simple-playback.c -o play
	
mixer-recoder: mixer-recoder.c
	gcc mixer-recoder.c -o mr