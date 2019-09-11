voip-answer.o: voip-answer.c
	cc -fPIC -O -DLIB -c -o $@ -Iinclude $<
