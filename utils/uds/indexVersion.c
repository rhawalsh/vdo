/*
 * Copyright Red Hat
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA. 
 *
 * $Id: //eng/uds-releases/krusty/src/uds/indexVersion.c#2 $
 */

#include "indexVersion.h"

void initialize_index_version(struct index_version *version,
			      uint32_t super_version)
{
	/*
	 * Version 1 was introduced for the first single file layout. It was
	 * used in RHEL7 and in RHEL8.0 Beta.  No kernel index ever used an
	 * earlier version.
	 */

	/*
	 * Version 2 was created when we discovered that the volume header page
	 * was written in native endian format.  It was used in RHEL8.0 and
	 * RHEL8.1.  We stopped reading and the volume header page, and changed
	 * to version 2 so that an index creaed on RHEL8 cannot be taken back
	 * an used on RHEL7.
	 *
	 * Versions 1 and 2 are identical in normal operation (i.e. after the
	 * index is loaded).
	 */

	/*
	 * Version 3 was created when we discovered the the chapter index
	 * headers were written in native endian format.  It was first used in
	 * RHEL8.2 and is the current version for new indices.
	 *
	 * Versions before 3 read and write native endian chapter headers.
	 * Version 3 reads chapter headers in any endian order, and writes
	 * little-endian chapter headers.
	 */
	bool chapter_index_header_native_endian = super_version < 3;

	*version = (struct index_version) {
		.chapter_index_header_native_endian =
			chapter_index_header_native_endian,
	};
}
