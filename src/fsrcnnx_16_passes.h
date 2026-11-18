/* GENERIERT von tools/gen_fsrcnnx.py aus FSRCNNX_x2_16-0-4-1.glsl - nicht von Hand ändern. */
#ifndef FSRCNNX_16_PASSES_H
#define FSRCNNX_16_PASSES_H

/* Rechen-Passes in Eingabegröße: Technique, Ausgabeplatz, Anzahl Eingaben, Eingabeplätze
 * (-1 = Quellbild "image", -2 = unbenutzt). Vorher COPY (Quellbild in ein eigenes Ziel), danach
 * AGG (liest Platz FSRCNNX_16_AGG_IN, schreibt Y' in 2x) und OUT (Zielgröße). */
#define FSRCNNX_16_SLOTS 12
#define FSRCNNX_16_PASSES 25
#define FSRCNNX_16_AGG_IN 3

static const struct fsrcnnx_pass fsrcnnx_16_passes[FSRCNNX_16_PASSES] = {
	{"P0", 0, 1, {-1, -2, -2, -2, -2}}, /* feature map 1 */
	{"P1", 1, 1, {-1, -2, -2, -2, -2}}, /* feature map 2 */
	{"P2", 2, 1, {-1, -2, -2, -2, -2}}, /* feature map 3 */
	{"P3", 3, 1, {-1, -2, -2, -2, -2}}, /* feature map 4 */
	{"P4", 4, 4, {0, 1, 2, 3, -2}}, /* mapping 1_1 */
	{"P5", 5, 4, {0, 1, 2, 3, -2}}, /* mapping 1_2 */
	{"P6", 6, 4, {0, 1, 2, 3, -2}}, /* mapping 1_3 */
	{"P7", 7, 4, {0, 1, 2, 3, -2}}, /* mapping 1_4 */
	{"P8", 8, 4, {4, 5, 6, 7, -2}}, /* mapping 2_1 */
	{"P9", 9, 4, {4, 5, 6, 7, -2}}, /* mapping 2_2 */
	{"P10", 10, 4, {4, 5, 6, 7, -2}}, /* mapping 2_3 */
	{"P11", 11, 4, {4, 5, 6, 7, -2}}, /* mapping 2_4 */
	{"P12", 4, 4, {8, 9, 10, 11, -2}}, /* mapping 3_1 */
	{"P13", 5, 4, {8, 9, 10, 11, -2}}, /* mapping 3_2 */
	{"P14", 6, 4, {8, 9, 10, 11, -2}}, /* mapping 3_3 */
	{"P15", 7, 4, {8, 9, 10, 11, -2}}, /* mapping 3_4 */
	{"P16", 8, 4, {4, 5, 6, 7, -2}}, /* mapping 4_1 */
	{"P17", 9, 4, {4, 5, 6, 7, -2}}, /* mapping 4_2 */
	{"P18", 10, 4, {4, 5, 6, 7, -2}}, /* mapping 4_3 */
	{"P19", 11, 4, {4, 5, 6, 7, -2}}, /* mapping 4_4 */
	{"P20", 4, 5, {8, 9, 10, 11, 0}}, /* sub-band residuals 1 */
	{"P21", 0, 5, {8, 9, 10, 11, 1}}, /* sub-band residuals 2 */
	{"P22", 1, 5, {8, 9, 10, 11, 2}}, /* sub-band residuals 3 */
	{"P23", 2, 5, {8, 9, 10, 11, 3}}, /* sub-band residuals 4 */
	{"P24", 3, 4, {4, 0, 1, 2, -2}}, /* sub-pixel convolution 1 */
};

#endif
