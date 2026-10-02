/* Геометрия кнопок смены месяца в календаре.
 *
 * Координаты взяты из оригинального ClearCalendarScreenlet.py
 * (detect_button / update_buttons), поэтому проверяем не «что мы
 * придумали», а совпадение с оригиналом:
 *
 *   полоса:  x 0..15, y 0..15   курсор в ней показывает кнопки
 *   кнопки:  y 5.5..12.5
 *            x  8.5..15.5  назад   (1)
 *            x 18.5..25.5  сегодня (2)
 *            x 28.5..35.5  вперёд  (3)
 *
 * Печатает по строке на контрольную точку: ожидание и что вернул
 * cal_button_at / cal_in_strip.
 */
#include <stdio.h>
#include <stdbool.h>

#define CAL_BTN_Y0      5.5
#define CAL_BTN_Y1     12.5
#define CAL_BTN_X0      8.5
#define CAL_BTN_W       7.0
#define CAL_BTN_GAP     3.0
#define CAL_BTN_STRIP_W 100.0  /* ширина полосы: во весь кадр */
#define CAL_BTN_STRIP_H  15.0  /* высота полосы: 15 юнитов */

static bool cal_in_strip(double x, double y)
{
	return x >= 0.0 && x < CAL_BTN_STRIP_W
	       && y >= 0.0 && y <= CAL_BTN_STRIP_H;
	/* x < CAL_BTN_STRIP, а не x < CAL_BTN_Y1: полоса во всю
	 * ширину кадра, как update_buttons в оригинале. */
}

static int cal_button_at(double x, double y)
{
	int i;

	if (y < CAL_BTN_Y0 || y > CAL_BTN_Y1)
		return 0;
	for (i = 0; i < 3; i++) {
		double bx = CAL_BTN_X0 + i * (CAL_BTN_W + CAL_BTN_GAP);

		if (x >= bx && x <= bx + CAL_BTN_W)
			return i + 1;
	}
	return 0;
}

int main(void)
{
	static const struct {
		double x, y;
		int btn;
		int strip;
		const char *what;
	} T[] = {
		/* середины кнопок оригинала */
		{ 12.0,  9.0, 1, 1, "назад, середина"},
		{ 22.0,  9.0, 2, 1, "сегодня, середина"},
		{ 32.0,  9.0, 3, 1, "вперёд, середина"},
		/* границы прямоугольников оригинала */
		{  8.5,  5.5, 1, 1, "назад, левый-верхний угол"},
		{ 15.5, 12.5, 1, 1, "назад, правый-нижний угол"},
		{ 28.5,  5.5, 3, 1, "вперёд, левый-верхний угол"},
		{ 35.5, 12.5, 3, 1, "вперёд, правый-нижний угол"},
		/* чуть за краями — не попадает */
		{  8.4,  9.0, 0, 1, "назад, левее на 0.1"},
		{ 35.6,  9.0, 0, 1, "вперёд, правее на 0.1"},
		{ 12.0,  5.4, 0, 1, "назад, выше на 0.1"},
		{ 12.0, 12.6, 0, 1, "назад, ниже на 0.1"},
		/* полоса видна, но кнопка не под курсором */
		{  3.0,  3.0, 0, 1, "пустое место полосы"},
		{  7.0, 14.0, 0, 1, "полоса, между кнопкой и краем"},
		/* вне полосы совсем */
		/* y вне полосы: кнопок нет, но полоса видна только пока
		 * курсор в y<=15, поэтому строка ниже y=40 вне полосы. */
		{ 20.0, 40.0, 0, 0, "сетка дня (ниже полосы)"},
		{ 60.0,  2.0, 0, 1, "справа от кнопок, но в полосе"},
		{120.0,  2.0, 0, 0, "за полосой"},
	};
	int fails = 0, i;
	unsigned n = sizeof T / sizeof T[0];

	for (i = 0; i < (int)n; i++) {
		int btn = cal_button_at(T[i].x, T[i].y);
		int strip = cal_in_strip(T[i].x, T[i].y) ? 1 : 0;
		int ok = (btn == T[i].btn) && (strip == T[i].strip);

		printf("  (%5.1f,%5.1f) btn=%d strip=%d  %-38s %s\n",
		       T[i].x, T[i].y, btn, strip, T[i].what, ok ? "OK" : "FAIL");
		if (!ok)
			fails++;
	}
	printf("\nпровалов: %d из %u\n", fails, n);
	return fails ? 1 : 0;
}