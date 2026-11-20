/* SPDX-License-Identifier: GPL-3.0-only */
/* SMK-37, Mewcoder (the vocoder build without the harmoniser: SMK_HARM_BUILD unset; smk37-devtools
 * vocoder-update-audit.md, "one source, two builds"). In smk_harm.h's place: what the shared code keeps of it, POLY CAP
 * (HOME > POLY CAP: the vocoder's too, voice.c). Everything else of the harmoniser is left out (#if SMK_HARM_BUILD) */
#define HM_PCAP_AUTO 6u              /* AUTO (the default, the owner 2026-11-20): 4 voices while the track's engine is FM6
                                      * (by far the costliest), no cap with any other */
#define HM_PCAP_FM6 4u
#define HM_PCAP_INIT {HM_PCAP_AUTO, HM_PCAP_AUTO, HM_PCAP_AUTO}
static uint8_t hm_pcap[3] = HM_PCAP_INIT;   /* POLY CAP (HOME > POLY CAP, harmoniser-spec.md §13w-2): T1..T3's
                                      * most voices while the VOCODER runs, 0 OFF (full), 1..5, AUTO; kept with the project
                                      * and the unit (vocset) */
#define HM_PCAP_MAX 5u
