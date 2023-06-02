/*
 * Copyright (c) 2023 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#ifndef DTLS_H_
#define DTLS_H_

int dtls_init(int sock);
int dtls_print_connection_id(int sock, bool verbose);
int dtls_save_session(int sock);
int dtls_load_session(int sock);

#endif /* DTLS_H_ */
