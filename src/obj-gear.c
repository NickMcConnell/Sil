/**
   \file obj-gear.c
   \brief management of inventory, equipment and quiver
 *
 * Copyright (c) 1997 Ben Harrison, James E. Wilson, Robert A. Koeneke
 * Copyright (c) 2014 Nick McConnell
 *
 * This work is free software; you can redistribute it and/or modify it
 * under the terms of either:
 *
 * a) the GNU General Public License as published by the Free Software
 *    Foundation, version 2, or
 *
 * b) the "Angband licence":
 *    This software may be copied and distributed for educational, research,
 *    and not for profit purposes provided that this copyright and statement
 *    are included in all such copies.  Other copyrights may also apply.
 */

#include "angband.h"
#include "obj-desc.h"
#include "obj-gear.h"
#include "obj-identify.h"
#include "obj-tval.h"
#include "obj-tvalsval.h"
#include "obj-util.h"
#include "player-util.h"
#include "spells.h"
#include "squelch.h"

static const struct slot_info {
	int index;
	bool acid_vuln;
	bool name_in_desc;
	const char *mention;
	const char *heavy_mention;
	const char *describe;
} slot_table[] = {
	#define EQUIP(a, b, c, d, e, f) { EQUIP_##a, b, c, d, e, f },
	#include "list-equip-slots.h"
	#undef EQUIP
	{ EQUIP_MAX, FALSE, FALSE, NULL, NULL, NULL }
};

/**
 * Convert a gear index into a one character label.
 *
 * Note that the label does NOT distinguish inven/equip.
 */
char index_to_label(int i)
{
	return I2A(i);
}


/**
 * Convert a label into the gear index of an item in the inventory.
 *
 * Return "-1" if the label does not indicate a real item.
 */
s16b label_to_inven(int c)
{
	int i;

	/* Convert */
	i = (islower((unsigned char)c) ? A2I(c) : -1);

	/* Verify the index */
	if ((i < 0) || (i > INVEN_PACK)) return (-1);

	/* Empty slots can never be chosen */
	if (!player->gear[player->upkeep->inven[i]].kind) return (-1);

	/* Return the index */
	return (player->upkeep->inven[i]);
}


/**
 * Convert a label into the gear index of an item in the equipment or quiver.
 *
 * Return "-1" if the label does not indicate a real item.
 */
s16b label_to_equip(int c)
{
	int i;

	/* Convert */
	i = (islower((unsigned char)c) ? A2I(c) : -1);

	/* Verify the index */
	if ((i < 0) || (i > player->body.count + QUIVER_SIZE)
		|| (i == player->body.count))
		return (-1);

	/* Equipment? */
	if (i < player->body.count && equipped_item_by_slot(player, i)->kind)
		return object_gear_index(player, equipped_item_by_slot(player, i));

	/* Quiver */
	i -= QUIVER_SIZE;

	/* Empty slots can never be chosen */
	if (!player->gear[player->upkeep->quiver[i]].kind) return (-1);

	/* Return the index */
	return player->upkeep->quiver[i];
}


/*
 * Hack -- determine if an item is "wearable" (or a missile)
 */
bool wearable_p(const object_type *o_ptr)
{
	return tval_is_wearable(o_ptr);
}


/**
 * Returns the number of times in 1000 that @ will FAIL
 * - thanks to Ed Graham for the formula
 */
int get_use_device_chance(const object_type *o_ptr)
{
	int lev, fail, numerator, denominator;

	int skill = player->state.skills[SKILL_DEVICE];

	int skill_min = 10;
	int skill_max = 141;
	int diff_min  = 1;
	int diff_max  = 100;

	/* Extract the item level, which is the difficulty rating */
	if (o_ptr->artifact)
		lev = o_ptr->artifact->level;
	else
		lev = o_ptr->kind->level;

	/* TODO: maybe use something a little less convoluted? */
	numerator   = (skill - lev) - (skill_max - diff_min);
	denominator = (lev - skill) - (diff_max - skill_min);

	/* Make sure that we don't divide by zero */
	if (denominator == 0) denominator = numerator > 0 ? 1 : -1;

	fail = (100 * numerator) / denominator;

	/* Ensure failure rate is between 1% and 75% */
	if (fail > 750) fail = 750;
	if (fail < 10) fail = 10;

	return fail;
}


/**
 * Distribute charges of rods, staves, or wands.
 *
 * \param o_ptr is the source item
 * \param q_ptr is the target item, must be of the same type as o_ptr
 * \param amt is the number of items that are transfered
 */
void distribute_charges(object_type *o_ptr, object_type *q_ptr, int amt)
{
	int charge_time = randcalc(o_ptr->time, 0, AVERAGE), max_time;

	/*
	 * Hack -- If rods, staves, or wands are dropped, the total maximum
	 * timeout or charges need to be allocated between the two stacks.
	 * If all the items are being dropped, it makes for a neater message
	 * to leave the original stack's pval alone. -LM-
	 */
	if (tval_can_have_charges(o_ptr))
	{
		q_ptr->pval = o_ptr->pval * amt / o_ptr->number;

		if (amt < o_ptr->number)
			o_ptr->pval -= q_ptr->pval;
	}

	/*
	 * Hack -- Rods also need to have their timeouts distributed.
	 *
	 * The dropped stack will accept all time remaining to charge up to
	 * its maximum.
	 */
	if (tval_can_have_timeout(o_ptr))
	{
		max_time = charge_time * amt;

		if (o_ptr->timeout > max_time)
			q_ptr->timeout = max_time;
		else
			q_ptr->timeout = o_ptr->timeout;

		if (amt < o_ptr->number)
			o_ptr->timeout -= q_ptr->timeout;
	}
}


void reduce_charges(object_type *o_ptr, int amt)
{
	/*
	 * Hack -- If rods or wand are destroyed, the total maximum timeout or
	 * charges of the stack needs to be reduced, unless all the items are
	 * being destroyed. -LM-
	 */
	if (tval_can_have_charges(o_ptr) && amt < o_ptr->number)
		o_ptr->pval -= o_ptr->pval * amt / o_ptr->number;

	if (tval_can_have_timeout(o_ptr) && amt < o_ptr->number)
		o_ptr->timeout -= o_ptr->timeout * amt / o_ptr->number;
}


int number_charging(const object_type *o_ptr)
{
	int charge_time, num_charging;

	charge_time = randcalc(o_ptr->time, 0, AVERAGE);

	/* Item has no timeout */
	if (charge_time <= 0) return 0;

	/* No items are charging */
	if (o_ptr->timeout <= 0) return 0;

	/* Calculate number charging based on timeout */
	num_charging = (o_ptr->timeout + charge_time - 1) / charge_time;

	/* Number charging cannot exceed stack size */
	if (num_charging > o_ptr->number) num_charging = o_ptr->number;

	return num_charging;
}


bool recharge_timeout(object_type *o_ptr)
{
	int charging_before, charging_after;

	/* Find the number of charging items */
	charging_before = number_charging(o_ptr);

	/* Nothing to charge */	
	if (charging_before == 0)
		return FALSE;

	/* Decrease the timeout */
	o_ptr->timeout -= MIN(charging_before, o_ptr->timeout);

	/* Find the new number of charging items */
	charging_after = number_charging(o_ptr);

	/* Return true if at least 1 item obtained a charge */
	if (charging_after < charging_before)
		return TRUE;
	else
		return FALSE;
}


int slot_by_name(struct player *p, const char *name)
{
	int i;

	/* Look for the correctly named slot */
	for (i = 0; i < p->body.count; i++)
		if (streq(name, p->body.slots[i].name)) break;

	/* Index for that slot */
	return i;
}

/**
 * Gets a slot of the given type, preferentially empty unless full is true
 */
int slot_by_type(struct player *p, int type, bool full)
{
	int i, fallback = p->body.count;

	/* Look for a correct slot type */
	for (i = 0; i < p->body.count; i++)
		if (type == p->body.slots[i].type) {
			if (full) {
				/* Found a full slot */
				if (p->body.slots[i].index != NO_OBJECT) break;
			} else {
				/* Found an empty slot */
				if (p->body.slots[i].index == NO_OBJECT) break;
			}
			/* Not right for full/empty, but still the right type */
			if (fallback == p->body.count)
				fallback = i;
		}

	/* Index for the best slot we found, or p->body.count if none found  */
	return (i != p->body.count) ? i : fallback;
}

bool slot_type_is(int slot, int type)
{
	return player->body.slots[slot].type == type ? TRUE : FALSE;
}

int slot_index(struct player *p, int slot)
{
	/* Check for valid slot */
	if (slot < 0 || slot >= p->body.count) return NO_OBJECT;

	/* Index into the gear array */
	return p->body.slots[slot].index;
}

struct object *equipped_item_by_slot(struct player *p, int slot)
{
	/* Index is set to NO_OBJECT if no object in that slot */
	return &p->gear[slot_index(p, slot)];
}

struct object *equipped_item_by_slot_name(struct player *p, const char *name)
{
	return equipped_item_by_slot(p, slot_by_name(p, name));
}

int equipped_item_slot(struct player *p, int item)
{
	int i;

	/* Look for an equipment slot with this item */
	for (i = 0; i < p->body.count; i++)
		if (item == p->body.slots[i].index) break;

	/* Correct slot, or p->body.count if not equipped */
	return i;
}

bool item_is_equipped(struct player *p, int item)
{
	return (equipped_item_slot(p, item) < p->body.count) ? TRUE : FALSE;
}

int object_gear_index(struct player *p, struct object *obj)
{
	int i;

	for (i = 0; i < MAX_GEAR; i++)
		if (obj == &p->gear[i]) break;

	return i;
}

int pack_slots_used(struct player *p)
{
	int i, quiver_slots = 0, pack_slots = 0, quiver_ammo = 0;
	int maxsize = MAX_STACK_SIZE - 1;

	for (i = 0; i < MAX_GEAR; i++) {
		struct object *obj = &p->gear[i];

		/* No actual object */
		if (!obj->kind) continue;

		/* Equipment doesn't count */
		if (item_is_equipped(p, i)) continue;

		/* Check if it could be in the quiver */
		if (tval_is_ammo(obj))
			if (quiver_slots < QUIVER_SIZE) {
				quiver_slots++;
				quiver_ammo += obj->number;
				continue;
			}

		/* Count regular slots */
		pack_slots++;
	}

	/* Full slots */
	pack_slots += quiver_ammo / maxsize;

	/* Plus one for any remainder */
	if (quiver_ammo % maxsize) pack_slots++;

	return pack_slots;
}

/*
 * Return a string mentioning how a given item is carried
 *
 * Need to deal with heavy weapon/bow - NRM
 */
const char *equip_mention(struct player *p, int slot)
{
	int type;

	//if (!item_is_equipped(item)) return "In pack";

	type = p->body.slots[slot].type;

	if (slot_table[type].name_in_desc)
		return format(slot_table[type].mention, p->body.slots[slot].name);
	else
		return slot_table[type].mention;
}


/*
 * Return a string describing how a given item is being worn.
 * Currently, only used for items in the equipment, not inventory.
 *
 * Need to deal with heavy weapon/bow - NRM
 */
const char *equip_describe(struct player *p, int slot)
{
	int type;

	//if (!item_is_equipped(item)) return NULL;

	type = p->body.slots[slot].type;

	if (slot_table[type].name_in_desc)
		return format(slot_table[type].describe, p->body.slots[slot].name);
	else
		return slot_table[type].describe;
}

/**
 * Determine which equipment slot (if any) an item likes. The slot might (or
 * might not) be open, but it is a slot which the object could be equipped in.
 *
 * For items where multiple slots could work (e.g. rings), the function
 * will try to return an open slot if possible.
 */
s16b wield_slot(const object_type *o_ptr)
{
	/* Slot for equipment */
	switch (o_ptr->tval)
	{
		case TV_BOW: return slot_by_type(player, EQUIP_BOW, FALSE);
		case TV_AMULET: return slot_by_type(player, EQUIP_AMULET, FALSE);
		case TV_CLOAK: return slot_by_type(player, EQUIP_CLOAK, FALSE);
		case TV_SHIELD: return slot_by_type(player, EQUIP_SHIELD, FALSE);
		case TV_GLOVES: return slot_by_type(player, EQUIP_GLOVES, FALSE);
		case TV_BOOTS: return slot_by_type(player, EQUIP_BOOTS, FALSE);
	}

	if (tval_is_melee_weapon(o_ptr))
		return slot_by_type(player, EQUIP_WEAPON, FALSE);
	else if (tval_is_ring(o_ptr))
		return slot_by_type(player, EQUIP_RING, FALSE);
	else if (tval_is_light(o_ptr))
		return slot_by_type(player, EQUIP_LIGHT, FALSE);
	else if (tval_is_body_armor(o_ptr))
		return slot_by_type(player, EQUIP_BODY_ARMOR, FALSE);
	else if (tval_is_head_armor(o_ptr))
		return slot_by_type(player, EQUIP_HAT, FALSE);

	/* No slot available */
	return (-1);
}


/*
 * Acid has hit the player, attempt to affect some armor.
 *
 * Note that the "base armor" of an object never changes.
 *
 * If any armor is damaged (or resists), the player takes less damage.
 */
int minus_ac(struct player *p)
{
	int i, count = 0;
	object_type *o_ptr = NULL;

	char o_name[80];

	/* Avoid crash during monster power calculations */
	if (!p->gear) return FALSE;

	/* Count the armor slots */
	for (i = 0; i < player->body.count; i++) {
		/* Ignore non-armor */
		if (slot_type_is(i, EQUIP_WEAPON)) continue;
		if (slot_type_is(i, EQUIP_BOW)) continue;
		if (slot_type_is(i, EQUIP_RING)) continue;
		if (slot_type_is(i, EQUIP_AMULET)) continue;
		if (slot_type_is(i, EQUIP_LIGHT)) continue;

		/* Add */
		count++;
	}

	/* Pick one at random */
	for (i = player->body.count - 1; i >= 0; i--) {
		/* Ignore non-armor */
		if (slot_type_is(i, EQUIP_WEAPON)) continue;
		if (slot_type_is(i, EQUIP_BOW)) continue;
		if (slot_type_is(i, EQUIP_RING)) continue;
		if (slot_type_is(i, EQUIP_AMULET)) continue;
		if (slot_type_is(i, EQUIP_LIGHT)) continue;

		if (one_in_(count--)) break;
	}

	/* Get the item */
	o_ptr = equipped_item_by_slot(player, i);

	/* Nothing to damage */
	if (!o_ptr->kind) return (FALSE);

	/* No damage left to be done */
	if (o_ptr->ac + o_ptr->to_a <= 0) return (FALSE);

	/* Describe */
	object_desc(o_name, sizeof(o_name), o_ptr, ODESC_BASE);

	/* Object resists */
	if (o_ptr->el_info[ELEM_ACID].flags & EL_INFO_IGNORE)
	{
		msg("Your %s is unaffected!", o_name);

		return (TRUE);
	}

	/* Message */
	msg("Your %s is damaged!", o_name);

	/* Damage the item */
	o_ptr->to_a--;

	p->upkeep->update |= PU_BONUS;
	p->upkeep->redraw |= (PR_EQUIP);

	/* Item was damaged */
	return (TRUE);
}

/*
 * Describe the charges on an item in the inventory.
 */
void inven_item_charges(int item)
{
	object_type *o_ptr = &player->gear[item];

	/* Require staff/wand */
	if (!tval_can_have_charges(o_ptr)) return;

	/* Require known item */
	if (!object_is_known(o_ptr)) return;

	/* Print a message */
	msg("You have %d charge%s remaining.", o_ptr->pval,
	    (o_ptr->pval != 1) ? "s" : "");
}


/*
 * Describe an item in the inventory. Note: only called when an item is 
 * dropped, used, or otherwise deleted from the inventory
 */
void inven_item_describe(int item)
{
	object_type *o_ptr = &player->gear[item];

	char o_name[80];

	if (o_ptr->artifact && 
		(object_is_known(o_ptr) || object_name_is_visible(o_ptr)))
	{
		/* Get a description */
		object_desc(o_name, sizeof(o_name), o_ptr, ODESC_FULL | ODESC_SINGULAR);

		/* Print a message */
		msg("You no longer have the %s (%c).", o_name, index_to_label(item));
	}
	else
	{
		/* Get a description */
		object_desc(o_name, sizeof(o_name), o_ptr, ODESC_PREFIX | ODESC_FULL);

		/* Print a message */
		msg("You have %s (%c).", o_name, index_to_label(item));
	}
}


/*
 * Increase the "number" of an item in the inventory
 */
void inven_item_increase(int item, int num)
{
	object_type *o_ptr = &player->gear[item];

	/* Apply */
	num += o_ptr->number;

	/* Bounds check */
	if (num > 255) num = 255;
	else if (num < 0) num = 0;

	/* Un-apply */
	num -= o_ptr->number;

	/* Change the number and weight */
	if (num)
	{
		/* Add the number */
		o_ptr->number += num;

		/* Add the weight */
		player->upkeep->total_weight += (num * o_ptr->weight);

		/* Recalculate bonuses */
		player->upkeep->update |= (PU_BONUS);

		/* Recalculate mana XXX */
		player->upkeep->update |= (PU_MANA);

		/* Recalculate gear */
		player->upkeep->update |= (PU_INVEN);

		/* Combine the pack */
		player->upkeep->notice |= (PN_COMBINE);

		/* Redraw stuff */
		player->upkeep->redraw |= (PR_INVEN | PR_EQUIP);
	}
}




/**
 * Erase an inventory slot if it has no more items
 */
void inven_item_optimize(int item)
{
	object_type *o_ptr = &player->gear[item];

	/* Only optimize real items which are empty */
	if (!o_ptr->kind || o_ptr->number) return;

	/* Stop tracking erased item if necessary */
	if (tracked_object_is(player->upkeep, item))
	{
		track_object(player->upkeep, NO_OBJECT);
	}

	/* Erase the empty slot */
	object_wipe(&player->gear[item]);
		
	/* Recalculate stuff */
	player->upkeep->update |= (PU_INVEN);
	player->upkeep->update |= (PU_BONUS);
	player->upkeep->update |= (PU_TORCH);
	player->upkeep->update |= (PU_MANA);

	/* Inventory has changed, so disable repeat command */ 
	cmd_disable_repeat();
}


/**
 * Check if we have space for an item in the pack without overflow
 */
bool inven_carry_okay(const object_type *o_ptr)
{
	/* Empty slot? */
	if (pack_slots_used(player) < INVEN_PACK) return TRUE;

	/* Check if it can stack */
	if (inven_stack_okay(o_ptr)) return TRUE;

	/* Nope */
	return FALSE;
}

/*
 * Check to see if an item is stackable in the inventory
 */
bool inven_stack_okay(const object_type *o_ptr)
{
	int j, new_number;
	bool extra_slot;

	/* Check for similarity */
	for (j = 0; j < MAX_GEAR; j++)
	{
		object_type *j_ptr = &player->gear[j];

		/* Skip equipped items and non-objects */
		if (item_is_equipped(player, j)) continue;
		if (!j_ptr->kind) continue;

		/* Check if the two items can be combined */
		if (object_similar(j_ptr, o_ptr, OSTACK_PACK)) break;
	}

	/* Definite no */
	if (j == MAX_GEAR) return FALSE;

	/* Add it and see what happens */
	player->gear[j].number += o_ptr->number;
	extra_slot = (player->gear[j].number > MAX_STACK_SIZE - 1);
	new_number = pack_slots_used(player);
	player->gear[j].number -= o_ptr->number;

	/* Analyse the results */
	if (new_number + (extra_slot ? 1 : 0) > INVEN_PACK) return FALSE;

	return TRUE;
}

/**
 * Add an item to the players inventory, and return the gear index used.
 *
 * If the new item can combine with an existing item in the inventory,
 * it will do so, using object_similar() and object_absorb(), else,
 * the item will be placed into the first available gear array index.
 *
 * This function can be used to "over-fill" the player's pack, but only
 * once, and such an action must trigger the "overflow" code immediately.
 * Note that when the pack is being "over-filled", the new item must be
 * placed into the "overflow" slot, and the "overflow" must take place
 * before the pack is reordered, but (optionally) after the pack is
 * combined.  This may be tricky.  See "dungeon.c" for info.
 *
 * Note that this code must remove any location/stack information
 * from the object once it is placed into the inventory.
 */
extern s16b inven_carry(struct player *p, struct object *o)
{
	int i, j;

	object_type *j_ptr;

	/* Apply an autoinscription */
	apply_autoinscription(o);

	/* Check for combining */
	for (j = 0; j < MAX_GEAR; j++)
	{
		j_ptr = &p->gear[j];

		if (!j_ptr->kind) continue;

		/* Check if the two items can be combined */
		if (object_similar(j_ptr, o, OSTACK_PACK))
		{
			/* Combine the items */
			object_absorb(j_ptr, o);

			/* Increase the weight */
			p->upkeep->total_weight += (o->number * o->weight);

			/* Recalculate bonuses */
			p->upkeep->update |= (PU_BONUS | PU_INVEN);

			/* Redraw stuff */
			p->upkeep->redraw |= (PR_INVEN);

			/* Success */
			return (j);
		}
	}

	/* Paranoia */
	if (pack_slots_used(p) > INVEN_PACK) return (-1);

	/* Find an empty slot */
	for (j = 0; j < MAX_GEAR; j++)
	{
		/* 0 is the NO_OBJECT slot.
		 * This line and comment are to emphasise that */
		if (j == NO_OBJECT) continue;

		j_ptr = &p->gear[j];
		if (!j_ptr->kind) break;
	}

	/* Use that slot */
	i = j;

	object_copy(&p->gear[i], o);

	j_ptr = &p->gear[i];

	/* Remove cave object details */
	j_ptr->next_o_idx = 0;
	j_ptr->held_m_idx = 0;
	j_ptr->iy = j_ptr->ix = 0;
	j_ptr->marked = FALSE;

	/* Update the inventory */
	p->upkeep->total_weight += (j_ptr->number * j_ptr->weight);
	p->upkeep->update |= (PU_BONUS | PU_INVEN);
	p->upkeep->notice |= (PN_COMBINE);
	p->upkeep->redraw |= (PR_INVEN);

	/* Hobbits ID mushrooms on pickup, gnomes ID wands and staffs on pickup */
	if (!object_is_known(j_ptr))
	{
		if (player_has(PF_KNOW_MUSHROOM) && tval_is_mushroom(j_ptr))
		{
			do_ident_item(j_ptr);
			msg("Mushrooms for breakfast!");
		}
		else if (player_has(PF_KNOW_ZAPPER) && tval_is_zapper(j_ptr))
		{
			do_ident_item(j_ptr);
		}
	}

	/* Return the slot */
	return (i);
}


/**
 * Take off (some of) a non-cursed equipment item
 *
 * Note that only one item at a time can be wielded per slot.
 *
 * Note that taking off an item when "full" may cause that item
 * to fall to the ground.
 *
 * Return the inventory slot into which the item is placed.
 */
void inven_takeoff(int item)
{
	int slot = equipped_item_slot(player, item);
	object_type *o_ptr;
	const char *act;
	char o_name[80];

	/* Paranoia */
	if (slot == player->body.count) return;

	/* Get the item to take off */
	o_ptr = &player->gear[item];

	/* Describe the object */
	object_desc(o_name, sizeof(o_name), o_ptr, ODESC_PREFIX | ODESC_FULL);

	/* Took off weapon */
	if (slot_type_is(slot, EQUIP_WEAPON))
	{
		act = "You were wielding";
	}

	/* Took off bow */
	else if (slot_type_is(slot, EQUIP_BOW))
	{
		act = "You were holding";
	}

	/* Took off light */
	else if (slot_type_is(slot, EQUIP_LIGHT))
	{
		act = "You were holding";
	}

	/* Took off something */
	else
	{
		act = "You were wearing";
	}

	/* De-equip the object */
	player->body.slots[equipped_item_slot(player, item)].index = NO_OBJECT;

	/* Message */
	msgt(MSG_WIELD, "%s %s (%c).", act, o_name, index_to_label(slot));

	player->upkeep->update |= (PU_BONUS | PU_INVEN);
	player->upkeep->notice |= PN_SQUELCH;

	return;
}


/**
 * Drop (some of) a non-cursed inventory/equipment item
 *
 * The object will be dropped "near" the current location
 */
void inven_drop(int item, int amt)
{
	int py = player->py;
	int px = player->px;

	object_type *o_ptr;

	object_type *i_ptr;
	object_type object_type_body;

	char o_name[80];

	/* Get the original object */
	o_ptr = &player->gear[item];

	/* Error check */
	if (amt <= 0) return;

	/* Not too many */
	if (amt > o_ptr->number) amt = o_ptr->number;

	/* Take off equipment */
	if (item_is_equipped(player, item))
		inven_takeoff(item);

	/* Stop tracking items no longer in the inventory */
	if (tracked_object_is(player->upkeep, item) && amt == o_ptr->number)
	{
		track_object(player->upkeep, NO_OBJECT);
	}

	i_ptr = &object_type_body;

	object_copy(i_ptr, o_ptr);
	object_split(i_ptr, o_ptr, amt);

	/* Describe local object */
	object_desc(o_name, sizeof(o_name), i_ptr, ODESC_PREFIX | ODESC_FULL);

	/* Message */
	msg("You drop %s (%c).", o_name, index_to_label(item));

	/* Drop it near the player */
	drop_near(cave, i_ptr, 0, py, px, FALSE);

	/* Modify, Describe, Optimize */
	inven_item_increase(item, -amt);
	inven_item_describe(item);
	inven_item_optimize(item);
}



/**
 * Return whether each stack of objects can be merged into two uneven stacks.
 */
static bool inventory_can_stack_partial(const object_type *o_ptr,
										const object_type *j_ptr,
										object_stack_t mode)
{
	if (!(mode & OSTACK_STORE)) {
		int total = o_ptr->number + j_ptr->number;
		int remainder = total - (MAX_STACK_SIZE - 1);

		if (remainder >= MAX_STACK_SIZE)
			return FALSE;
	}

	return object_stackable(o_ptr, j_ptr, mode);
}

/**
 * Combine items in the pack
 * Also pick up any gold in the inventory by accident
 */
void combine_pack(void)
{
	int i, j;
	object_type *o_ptr;
	object_type *j_ptr;
	bool display_message = FALSE;
	bool redraw = FALSE;

	/* Combine the pack (backwards) */
	for (i = MAX_GEAR - 1; i >= 0; i--)
	{
		/* Get the item */
		o_ptr = &player->gear[i];

		/* Skip empty items */
		if (!o_ptr->kind) continue;

		/* Absorb gold */
		if (tval_is_money(o_ptr))
		{
			/* Count the gold */
			player->au += o_ptr->pval;
			object_wipe(o_ptr);
		}

		/* Scan the items above that item */
		else for (j = 0; j < i; j++)
		{
			/* Get the item */
			j_ptr = &player->gear[j];

			/* Skip empty items */
			if (!j_ptr->kind) continue;

			/* Can we drop "o_ptr" onto "j_ptr"? */
			if (object_similar(j_ptr, o_ptr, OSTACK_PACK)) {
				display_message = TRUE;
				redraw = TRUE;
				object_absorb(j_ptr, o_ptr);
				break;
			}
			else if (inventory_can_stack_partial(j_ptr, o_ptr, OSTACK_PACK)) {
				/* Setting this to TRUE spams the combine message. */
				display_message = FALSE;
				redraw = TRUE;
				object_absorb_partial(j_ptr, o_ptr);
				break;
			}
		}
	}

	/* Redraw stuff */
	if (redraw) {
		player->upkeep->redraw |= (PR_INVEN);
		player->upkeep->update |= (PU_INVEN);
	}

	/* Message */
	if (display_message)
	{
		msg("You combine some items in your pack.");

		/* Stop "repeat last command" from working. */
		cmd_disable_repeat();
	}
}

/**
 * Returns whether the pack is holding the maximum number of items.
 */
bool pack_is_full(void)
{
	return pack_slots_used(player) == INVEN_PACK ? TRUE : FALSE;
}

/**
 * Returns whether the pack is holding the more than the maximum number of
 * items. If this is true, calling pack_overflow() will trigger a pack overflow.
 */
bool pack_is_overfull(void)
{
	return pack_slots_used(player) > INVEN_PACK ? TRUE : FALSE;
}

/**
 * Overflow an item from the pack, if it is overfull.
 */
void pack_overflow(void)
{
	int item = player->upkeep->inven[INVEN_PACK];
	char o_name[80];
	object_type *o_ptr;

	if (!pack_is_overfull()) return;

	/* Get the slot to be dropped */
	o_ptr = &player->gear[item];

	/* Disturbing */
	disturb(player, 0);

	/* Warning */
	msg("Your pack overflows!");

	/* Describe */
	object_desc(o_name, sizeof(o_name), o_ptr, ODESC_PREFIX | ODESC_FULL);

	/* Message */
	msg("You drop %s (%c).", o_name, index_to_label(item));

	/* Drop it (carefully) near the player */
	drop_near(cave, o_ptr, 0, player->py, player->px, FALSE);

	/* Modify, Describe, Optimize */
	inven_item_increase(item, -255);
	inven_item_describe(item);
	inven_item_optimize(item);

	/* Notice stuff (if needed) */
	if (player->upkeep->notice) notice_stuff(player->upkeep);

	/* Update stuff (if needed) */
	if (player->upkeep->update) update_stuff(player->upkeep);

	/* Redraw stuff (if needed) */
	if (player->upkeep->redraw) redraw_stuff(player->upkeep);
}
