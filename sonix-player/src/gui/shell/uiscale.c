#include "uiscale.h"

#include <stdio.h>

int ui_scale_num = 1;
int ui_scale_den = 1;

void ui_scale_set(int num, int den) {
	if (num <= 0 || den <= 0) {
		num = den = 1;
	}
	ui_scale_num = num;
	ui_scale_den = den;
	if (num != den) {
		printf("ui: laid out at %d/%d of the 480-wide design\n", num, den);
	}
}
