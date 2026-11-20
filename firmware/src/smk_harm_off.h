/* SPDX-License-Identifier: GPL-3.0-only */
/* SMK-37, Mewcoder (the vocoder build without the harmoniser: SMK_HARM_BUILD unset; smk37-devtools
 * vocoder-update-audit.md, "one source, two builds"). In smk_harm.h's place: what the shared code keeps of it, POLY CAP
 * (HOME > POLY CAP: the vocoder's too, voice.c). Everything else of the harmoniser is left out (#if SMK_HARM_BUILD) */
static uint8_t hm_pcap[3];           /* POLY CAP (HOME > POLY CAP, harmoniser-spec.md §13w-2): T1..T3's most voices while
                                      * the VOCODER runs, 0 OFF (full), 1..5; kept with the project and the unit (vocset) */
#define HM_PCAP_MAX 5u
