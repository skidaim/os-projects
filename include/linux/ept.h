/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_EPT_H
#define _LINUX_EPT_H

#include <linux/mm_types.h>

void ept_invalidate(struct mm_struct *mm, unsigned long address);

#endif /* _LINUX_EPT_H */
