static void dump_rect(struct dcp_rect r)
{
	printk("\t\t\t(%u, %u) -> (%u, %u)\n", r.x, r.y, r.x + r.w, r.y + r.h);
}

static void dump_swap_rec(struct dcp_iomfbswaprec *r)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(r->unk0); ++i)
		WARN_ON(r->unk0[i] != 0);

	for (i = 0; i < ARRAY_SIZE(r->unk1); ++i)
		WARN_ON(r->unk1[i] != 0);

	WARN_ON(r->flags1);
	WARN_ON(r->flags2);

	printk("\tSwap ID: %u\n", r->swap_id);
	printk("\tSwap enabled: %u\n", r->swap_enabled);
	printk("\tSwap completed: %u\n", r->swap_completed);

	for (i = 0; i < SWAP_SURFACES; ++i) {
		printk("\tSurface %u:\n", r->surf_ids[i]);
		printk("\t\tFlags %u:\n", r->surf_flags[i]);
		printk("\t\tSource rect:\n");
		dump_rect(r->src_rect[i]);
		printk("\t\tDestination rect:\n");
		dump_rect(r->dst_rect[i]);

		WARN_ON(r->surf_unk[i]);
	}
}

static void dump_iosurface(struct dcp_iosurface *r)
{
	printk("\tTiled: %u\n", r->is_tiled);
	printk("\tPlane count 1: %u\n", r->plane_cnt);
	printk("\tPlane count 2: %u\n", r->plane_cnt2);
	printk("\tFormat: %08X\n", r->format);

	printk("\tStride: %u\n", r->stride);
	printk("\tPixel size: %u\n", r->pix_size);
	printk("\tPixel element width: %u\n", r->pel_w);
	printk("\tPixel element height: %u\n", r->pel_h);
	printk("\tOffset: %u\n", r->offset);
	printk("\tWidth: %u\n", r->width);
	printk("\tHeight: %u\n", r->height);
	printk("\tBuffer size: %u\n", r->buf_size);
	printk("\tUnk 1: %u\n", r->unk_1);
	printk("\tUnk 2: %u\n", r->unk_2);
	printk("\tUnk F: %u\n", r->unk_f);
	printk("\tUnk 13: %u\n", r->unk_13);
	printk("\tUnk 14: %u\n", r->unk_14);
	printk("\tUnk 14: %u\n", r->unk_14);

	/* TODO: finish */
}

static void dump_swap_submit_req(struct dcp_swap_submit_req *r)
{
	int i;

	dump_swap_rec(&r->swap_rec);

	for (i = 0; i < SWAP_SURFACES; ++i) {
		printk("Surface %d:\n", i);
		dump_iosurface(&r->surf[i]);
		printk("\tIOVA: 0x%X\n", r->surf_iova[i]);
		printk("\tNull:%u\n", r->surf_null[i]);
	}

	printk("Unboolk %u\n", r->unkbool);
	printk("Unkdouble %llu\n", r->unkdouble);
	printk("Unkint %u\n", r->unkint);
	printk("Swap rec null %u\n", r->swap_rec_null);
	printk("Unkout_bool null %u\n", r->unkoutbool_null);
}
