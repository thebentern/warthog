/* SPDX-License-Identifier: GPL-2.0-or-later
 * Trivial driver for the dot11ah link check — exists only so the linker pulls
 * every ported unit together and reports any duplicate/unresolved symbol.
 * (The dot11ah module init/exit are not kernel hooks here; nothing calls them.) */
int main(void) { return 0; }
