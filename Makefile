voip-answer: voip-answer.c
	cc -fPIC -O -DLIB -o $@ -Iinclude $< -lpopt
