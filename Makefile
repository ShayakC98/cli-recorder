all: simple-playback.c mixer-recoder.c
	gcc simple-playback.c -o play -lm
	gcc mixer-recoder.c -o mr -lm

playback: simple-playback.c
	gcc simple-playback.c -o play -lm
	
mixer-recoder: mixer-recoder.c
	gcc mixer-recoder.c -o mr -lm