/*
    This file is part of Kismet

    Kismet is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    Kismet is distributed in the hope that it will be useful,
      but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Kismet; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

/* Functions the vendored mt76 USB code references from mt76 files that aren't vendored
 * (MMIO DMA, PCI and debugfs).  Kept outside the driver so that directory stays an
 * unmodified copy. */

#include "mt76.h"
#include "mt792x.h"
#include "mt7921/mt7921.h"
#include "mt7921/spectrum.h"

/* From mt76 dma.c (BSD-3-Clause-Clear) */
void
mt76_put_txwi(struct mt76_dev *dev, struct mt76_txwi_cache *t)
{
	if (!t)
		return;

	spin_lock(&dev->lock);
	list_add(&t->list, &dev->txwi_cache);
	spin_unlock(&dev->lock);
}

/* From mt76 mmio.c; USB devices have no interrupt mask register */
void mt76_set_irq_mask(struct mt76_dev *dev, u32 addr, u32 clear, u32 set) {
    (void) dev; (void) addr; (void) clear; (void) set;
}

/* PCI only; USB devices never need a WPDMA reinit */
int mt792x_wpdma_reinit_cond(struct mt792x_dev *dev) {
    (void) dev;
    return 0;
}

/* No debugfs, but debugfs setup is where the I/Q capture and its character device start */
int mt7921_init_debugfs(struct mt792x_dev *dev) {
    return mt7921_spectrum_init(dev, NULL);
}
