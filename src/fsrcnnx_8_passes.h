/* GENERIERT von tools/gen_fsrcnnx.py aus FSRCNNX_x2_8-0-4-1.glsl - nicht von Hand ändern. */
#ifndef FSRCNNX_8_PASSES_H
#define FSRCNNX_8_PASSES_H

/* Rechen-Passes in Eingabegröße: Technique, Ausgabeplatz, Anzahl Eingaben, Eingabeplätze
 * (-1 = Quellbild "image", -2 = unbenutzt). Vorher COPY (Quellbild in ein eigenes Ziel), danach
 * AGG (liest Platz FSRCNNX_8_AGG_IN, schreibt Y' in 2x) und OUT (Zielgröße). */
#define FSRCNNX_8_SLOTS 6
#define FSRCNNX_8_PASSES 13
#define FSRCNNX_8_AGG_IN 1

static const struct fsrcnnx_pass fsrcnnx_8_passes[FSRCNNX_8_PASSES] = {
	{"P0", 0, 1, {-1, -2, -2, -2, -2}}, /* feature map 1 */
	{"P1", 1, 1, {-1, -2, -2, -2, -2}}, /* feature map 2 */
	{"P2", 2, 2, {0, 1, -2, -2, -2}}, /* mapping 1_1 */
	{"P3", 3, 2, {0, 1, -2, -2, -2}}, /* mapping 1_2 */
	{"P4", 4, 2, {2, 3, -2, -2, -2}}, /* mapping 2_1 */
	{"P5", 5, 2, {2, 3, -2, -2, -2}}, /* mapping 2_2 */
	{"P6", 2, 2, {4, 5, -2, -2, -2}}, /* mapping 3_1 */
	{"P7", 3, 2, {4, 5, -2, -2, -2}}, /* mapping 3_2 */
	{"P8", 4, 2, {2, 3, -2, -2, -2}}, /* mapping 4_1 */
	{"P9", 5, 2, {2, 3, -2, -2, -2}}, /* mapping 4_2 */
	{"P10", 2, 3, {4, 5, 0, -2, -2}}, /* sub-band residuals 1 */
	{"P11", 0, 3, {4, 5, 1, -2, -2}}, /* sub-band residuals 2 */
	{"P12", 1, 2, {2, 0, -2, -2, -2}}, /* sub-pixel convolution 1 */
};

#endif
