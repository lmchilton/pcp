/*
 * CAVEAT
 *	The interfaces and data structures defined in this header are
 *	deployed via the libpcp_archive library and intended for
 *	Performance Co-Pilot (PCP) developers working with the few
 *	apps that are able to write PCP archives.
 *
 *	They are not part of the PCP APIs that are guaranteed to
 *	remain fixed across releases, and they may not work, or may
 *	provide different semantics at some point in the future.
 *
 * Copyright (c) 2022 Ken McDonell.  All Rights Reserved.
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public
 * License for more details.
 */
#ifndef PCP_ARCHIVE_H
#define PCP_ARCHIVE_H

#ifdef __cplusplus
extern "C" {
#endif

PCP_CALL extern void pmaSortIndom(pmInResult *);
PCP_CALL extern int pmaSameIndom(pmInResult *, pmInResult *);
PCP_CALL extern int pmaDeltaIndom(pmInResult *, pmInResult *, pmInResult *);

#ifdef __cplusplus
}
#endif

#endif /* PCP_ARCHIVE_H */
