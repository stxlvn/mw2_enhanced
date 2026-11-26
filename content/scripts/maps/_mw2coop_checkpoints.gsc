// mw2coop: checkpoints for the levels that run their sections themselves (Wolverines!: start_bmp
// threads handler_house_destroyer_to_pizza, which the section before calls when it ends). A start
// point is reached when a flag of the level is set: the last flag that start point's start
// function sets and the one before it does not, i.e. the end of the section before it, set in a
// normal run by a script or a map trigger. Found with tools/find_section_handlers.py and checked
// by hand; developers' test starts, generic helpers and flags that may come early are left out.
// Levels whose sections are add_start() section functions need no table (maps\_load patch), and
// the start points marked only by a section handler are hooked by tools/gen_checkpoint_patches.py.
// Each flag reached calls maps\_mw2coop::section_reached, which only moves forward.
// Audit of 2026-11-08 (issue #16 and the Arcadia report in issue #9): the rule "the flag the start function sets" picked flags that are set long
// before the start point's place (Arcadia's used_laser at the first laser use, disable_stryker_dialog right after the checkpoint), and some
// sections begin before the players get there (late_sections). Every pair below was checked against the map data (where the flag's trigger is
// against the place the start point puts the players: a restart may put them a little back, never ahead) and, where a real session's log had
// it, against where both players were when the checkpoint was taken.
#include common_scripts\utility;
#include maps\_utility;

// [ flag, start point ] pairs of a level.
table( level_name )
{
	t = [];
	switch ( level_name )
	{
		case "favela":
			t[ t.size ] = pair( "start_chase", "chase" );
			t[ t.size ] = pair( "favela_music", "favela" );
			// the lower favela is cleared (the trigger is 450 units from playerstart_soccer), the final bend below the hilltop (180 from playerstart_hilltop) and the
			// foot of the final stairs (100 from playerstart_end): without them a death near the end of the mission restarted the torture scene (issue #9)
			t[ t.size ] = pair( "cleared_favela", "soccer" );
			t[ t.size ] = pair( "player_at_final_bend", "hilltop" );
			t[ t.size ] = pair( "player_approaching_final_stairs", "end" );
			break;
		case "arcadia":
			// the game's own "past the checkpoint" trigger, 877 units past playerstart_checkpoint (was used_laser, set the first time a player used the
			// laser in the opening street fight: a death there restarted 9600 units ahead at Checkpoint Lima, the squad far behind; issue #9)
			t[ t.size ] = pair( "past_checkpoint", "checkpoint" );
			t[ t.size ] = pair( "golf_course_battle", "golf" );
			// approaching the house, 645 units past playerstart_crash (was disable_stryker_dialog, whose trigger is right after Checkpoint Lima: the end
			// of the mission became the checkpoint there; issue #9, "teleported to the very end")
			t[ t.size ] = pair( "player_approaching_house", "crash" );
			break;
		case "oilrig":
			t[ t.size ] = pair( "player_breaks_surface", "surface" );
			t[ t.size ] = pair( "player_ready_to_be_helped_from_water", "rig" );
			t[ t.size ] = pair( "lower_room_breached", "deck1" );
			t[ t.size ] = pair( "player_approaching_deck_2", "deck2" );
			t[ t.size ] = pair( "obj_explosives_locate_given", "deck3" );
			break;
		case "favela_escape":
			// "street" is reached by its place (places): the second technical arrives at the radio tower 1200 units and half a minute before it
			t[ t.size ] = pair( "vista1_dialogue_start", "street_mid" );
			t[ t.size ] = pair( "leftalley_dialogue_start", "street_vista2" );
			t[ t.size ] = pair( "market_advance_1", "market" );
			// the market evacuation's own start (market_evac() waits for it), 166 units from the start point's place (was market_advance_4, 1550 units before it)
			t[ t.size ] = pair( "market_evac_start", "market_evac" );
			t[ t.size ] = pair( "player_near_bigjump", "roofrun_bigjump" );
			t[ t.size ] = pair( "solorun_objective_display", "solorun" );
			t[ t.size ] = pair( "solorun_player_near_balcony", "solorun_rooftops" );
			// the slide down to the chopper, 486 units from the place the start point puts the player (was the balcony, 4330 units and the whole rooftop run before it)
			t[ t.size ] = pair( "trig_solorun_player_on_slide", "solorun_chopper" );
			break;
		case "dcburning":
			t[ t.size ] = pair( "obj_commerce_given", "elevator_bottom" );
			t[ t.size ] = pair( "player_at_commerce_crows_floor", "elevator_top" );
			t[ t.size ] = pair( "player_approaching_crowsnest2", "crows_nest" );
			t[ t.size ] = pair( "start_crow_armor_sequence", "crows_nest_armor" );
			t[ t.size ] = pair( "crowsnest_sequence_finished", "to_roof" );
			t[ t.size ] = pair( "player_approaching_outer_balcony", "roof" );
			t[ t.size ] = pair( "player_crash_done", "crash" );
			break;
		case "contingency":
			t[ t.size ] = pair( "start_btr_slide", "slide" );
			// safe from the BTRs, 650 units past woods_start_player (it was the midwoods checkpoint, 4660 units before midwoods_start_player); the woods'
			// section handler begins earlier (late_sections)
			t[ t.size ] = pair( "safe_from_btrs", "woods" );
			// the woods' stealth area 9, 585 units past midwoods_start_player (the nearer dialog_woods_blocking_stationary is set off by Price, not by a player:
			// its trigger's spawnflags are AI_ALLIES + NOTPLAYER; QA cpwalk)
			t[ t.size ] = pair( "area_9", "midwoods" );
			// the ridge itself, 110 units from ridge_start_player: handle_midwoods() starts the ridge's handler at once, so the ridge was the checkpoint
			// from the middle of the woods on (owner's log of 2026-11-04: taken 7400 units and 160 s before the ridge)
			t[ t.size ] = pair( "approaching_ridge", "ridge" );
			t[ t.size ] = pair( "leaving_village", "base" );
			t[ t.size ] = pair( "price_splits_off", "defend_sub" );
			break;
		case "boneyard":
			// the sections lie along +x; each flag's trigger is 180 to 920 units past its start point's place (a respawn puts the players a little back, never ahead)
			t[ t.size ] = pair( "road", "road" );
			t[ t.size ] = pair( "flyby_rockets", "flyby" );
			t[ t.size ] = pair( "higround_ascend", "higround" );
			t[ t.size ] = pair( "uaz_driver_mount", "ride" );
			// no ride_end: its flag was uaz_mounted, set when a player climbs into the UAZ at the start of the ride, so a death anywhere on the ride
			// restarted at its end; the map has no flag near the ride's end, so the ride's checkpoint is its start
			break;
		case "af_caves":
			t[ t.size ] = pair( "rappel_threads", "rappel" );
			// both players are down the rope (_mw2coop_cave), not when the first starts the knife kill
			t[ t.size ] = pair( "mw2coop_cave_down", "barracks" );
			t[ t.size ] = pair( "player_pre_steamroom_stairs", "steamroom" );
			t[ t.size ] = pair( "steamroom_ambush_finish_dialogue_ended", "ledge" );
			t[ t.size ] = pair( "player_crossed_bridge", "overlook" );
			t[ t.size ] = pair( "player_right_near_breach", "control" );
			t[ t.size ] = pair( "obj_escape_complete", "airstrip" );
			break;
		case "estate":
			// "ambush" is reached by its place (places): print_first_objective came 9 s after the landing, 2000 units before it
			// the mines in the forest, 590 units past the forest fight's start point (it had no checkpoint)
			t[ t.size ] = pair( "bouncing_betty_activated", "forest_fight" );
			// the house ahead, 580 units past player_houseapproach_start (was player_is_out_of_ambush_zone, 2100 units before it)
			t[ t.size ] = pair( "approaching_house", "house_approach" );
			// at the house, 220 units from breach_tweak_start (was deploy_house_defense_jeeps, 1230 units before it)
			t[ t.size ] = pair( "house_perimeter_softened", "house_breach" );
			t[ t.size ] = pair( "all_enemies_killed_up_to_house_capture", "house_briefing" );
			t[ t.size ] = pair( "ozone_to_earlydefense_start", "house_defense" );
			t[ t.size ] = pair( "dsm_recovered", "escape" );
			t[ t.size ] = pair( "finish_line", "ending" );
			break;
		case "dc_whitehouse":
			t[ t.size ] = pair( "whitehouse_entrance_clear", "oval_office" );
			// "flare" is reached by its place (places): the countdown's trigger is 1850 units before it
			break;
		case "airport":
			// the massacre is over (every group of it dead, set by airport::massacre_main itself): massacre_main() returns at its first trigger, so the
			// gate was the checkpoint during the massacre (owner's log: taken 0.5 s after "massacre", 58 s before the players reached the gate)
			t[ t.size ] = pair( "gate_main", "gate" );
			break;
		case "gulag":
			// outside the cell blocks, 230 units from the control room start point: the section begins when the players land from the helicopter, 80 s
			// of fighting before it (owner's log)
			t[ t.size ] = pair( "postup_outside_gulag", "control_room" );
			break;
		case "dcemp":
			// the office's first ally move, 130 units from the office start point: lobby_main() ends right after the lobby's move-out (owner's log: the
			// office was the checkpoint 1.3 s after the lobby, 33 s before the players reached it)
			t[ t.size ] = pair( "office_ally_color_2", "office" );
			break;
		case "roadkill":
			// riverbank's function only starts threads, so handle_starts enters "convoy" with the level (not a checkpoint then): it is reached at the foot of the stairs up to
			// the roof fight, the trigger 54 units from the place start_convoy puts the player (start_ride_player)
			t[ t.size ] = pair( "player_starts_stairs", "convoy" );
			break;
		case "af_chase":
			// Endgame: the boat sections' functions are empty (::empty), so handle_starts marks none of them (issue #16). Each is reached where single player saves or just past
			// the place its start point puts the boat (map data): boarding (single player's "mount_boat" save), the cave exit (its save trigger, 460 units past
			// lake_start_pose), the rapids (their save trigger), the river below them (4170 units past below_rapids_start_position) and the boarding scene ahead
			// (3000 past end_start_position). The middle of the lake has no flag: places().
			t[ t.size ] = pair( "player_on_boat", "boatdrive_begin" );
			t[ t.size ] = pair( "exit_caves", "boatdrive_lake" );
			t[ t.size ] = pair( "rapids_trigger", "boatdrive_rapids" );
			t[ t.size ] = pair( "on_river", "boatdrive_below_rapids" );
			t[ t.size ] = pair( "player_in_sight_of_boarding", "boatdrive_end" );
			break;
		case "invasion":
			// the house destroyer BMP's street, 220 units from the bmp start point (its section handler begins earlier: late_sections)
			t[ t.size ] = pair( "start_house_destroyer", "bmp" );
			t[ t.size ] = pair( "spawn_nates_attackers_in_alley", "pizza" );
			// the gas station and the crash site are reached by their places (places); leaving_gas_station, which marked the crash site, is 3200 units before it
			t[ t.size ] = pair( "crash_objective", "nates_roof" );
			t[ t.size ] = pair( "sentry_in_position", "attack_diner" );
			t[ t.size ] = pair( "nates_bomb_incoming", "diner" );
			t[ t.size ] = pair( "taco_goes_to_roof", "vip_escort" );
			// start point names are lower case (maps\_utility::add_start lowers them): "defend_BT" never matched, so the defence of Burger Town had no checkpoint
			t[ t.size ] = pair( "president_in_BT_meat_locker", "defend_bt" );
			t[ t.size ] = pair( "first_attack_heli_spawned", "helis" );
			break;
	}
	return t;
}

pair( flag_name, start_name )
{
	p = [];
	p[ 0 ] = flag_name;
	p[ 1 ] = start_name;
	return p;
}

// [ script_struct (or entity) targetname, start point ] pairs of a level: the start point is reached when a player passes the place it puts him (in front of
// it along its heading, within 2500 units of it). For sections with no flag of the level near their start.
places( level_name )
{
	t = [];
	switch ( level_name )
	{
		case "af_chase":
			// the boat start in the middle of the lake (its nearest flag trigger is 19700 units back)
			t[ t.size ] = pair( "lake_mid_start_pose", "boatdrive_lake_mid" );
			break;
		case "invasion":
			// the start points' own places: the gas station's section handler begins at the truck, 1700 units and a minute before it (owner's logs)
			t[ t.size ] = pair( "start_gas_station", "gas_station" );
			t[ t.size ] = pair( "start_crash", "crash" );
			break;
		case "favela_escape":
			t[ t.size ] = pair( "struct_start_street", "street" );
			break;
		case "estate":
			// a script_origin, facing the ambush
			t[ t.size ] = pair( "player_ambush_start", "ambush" );
			break;
		case "dc_whitehouse":
			t[ t.size ] = pair( "flare_start_points", "flare" );
			break;
	}
	return t;
}

// Sections of a level whose own beginning (its add_start() section function in maps\_load::handle_starts, or the level's section handler, patched)
// comes before the players get there: the function of the section before returns, or the handler is called, while that section still runs.
// maps\_mw2coop::section_reached ignores that beginning; the flag or the place in table() / places() marks the section.
late_sections( level_name )
{
	switch ( level_name )
	{
		case "airport":
			return strtok( "gate", " " );
		case "contingency":
			return strtok( "woods ridge", " " );
		case "gulag":
			return strtok( "control_room", " " );
		case "dcemp":
			return strtok( "office", " " );
		case "invasion":
			return strtok( "bmp gas_station", " " );
	}
	return [];
}

// Levels whose flags kept at a checkpoint are not set again after a restart at its start point (maps\_mw2coop restore_flags). Favela: every thread of the level
// that waits on an earlier flag begins a scene the players left behind (start_chase: the chase with the runner, who got away at once and failed the mission, owner's
// log 2026-11-26; visionset_chase / visionset_torture: the vision of the chase or the torture over the favela's; faust_appearance_1..4: Faust and the objective marker
// on him behind the players, with a save each; the rooftop dialogue said again), and its start functions set what their own section needs: a session went from the
// start "favela" to the end of the mission with none of them set again.
no_flag_restore( level_name )
{
	return level_name == "favela";
}

// From maps\_mw2coop::main() in a co-op campaign session.
watch_flags()
{
	foreach ( p in table( level.script ) )
		level thread watch_flag( p[ 0 ], p[ 1 ] );
	foreach ( p in places( level.script ) )
		level thread watch_place( p[ 0 ], p[ 1 ] );
}

watch_place( struct_name, start_name )
{
	level endon( "mw2coop_level_end" );
	wait 0.1;	// maps\_load sets up the structs (getstruct) after maps\_mw2coop::main()
	// the first of them (a start point may have one for each person it places, all facing the same way: Favela Escape's struct_start_street;
	// getstruct() gives none when there are several)
	spot = place_of( struct_name );
	if ( !isdefined( spot ) )
	{
		maps\_mw2coop::state( "checkpoints: place " + struct_name + " (" + start_name + ") not in this level" );
		return;
	}
	yaw = 0;
	if ( isdefined( spot.angles ) )
		yaw = spot.angles[ 1 ];
	forward = AnglesToForward( ( 0, yaw, 0 ) );
	for ( ;; )
	{
		wait 0.1;
		foreach ( player in level.players )
		{
			if ( !IsAlive( player ) || Distance2D( player.origin, spot.origin ) > 2500 )
				continue;
			d = player.origin - spot.origin;
			if ( d[ 0 ] * forward[ 0 ] + d[ 1 ] * forward[ 1 ] > 0 )
			{
				maps\_mw2coop::state( "checkpoints: " + player_text( player ) + " passed " + struct_name );
				maps\_mw2coop::section_reached( start_name, player, true );
				return;
			}
		}
	}
}

// The struct of that targetname (the first of them: a start point may have one for each person it places, all facing the same way; getstruct()
// gives none when there are several), or else the entity (Estate's player_ambush_start is a script_origin).
place_of( name )
{
	spots = getstructarray( name, "targetname" );
	if ( spots.size )
		return spots[ 0 ];
	return GetEnt( name, "targetname" );
}

player_text( player )
{
	if ( player == level.player )
		return "player 1";
	return "player 2";
}

watch_flag( flag_name, start_name )
{
	level endon( "mw2coop_level_end" );
	for ( t = 0; !isdefined( level.flag[ flag_name ] ); t += 0.5 )
	{
		if ( t == 30 )
			maps\_mw2coop::state( "checkpoints: flag " + flag_name + " (" + start_name + ") not in this level yet" );
		wait 0.5;
	}
	flag_wait( flag_name );
	// the player in the flag's trigger got there first (the other may be brought to him)
	leader = undefined;
	// GetEntArray( flag, "script_flag" ) finds nothing in this engine: pick by the field
	foreach ( trigger in array_combine( GetEntArray( "trigger_multiple_flag_set_touching", "classname" ), GetEntArray( "trigger_multiple_flag_set", "classname" ) ) )
		if ( isdefined( trigger.script_flag ) && trigger.script_flag == flag_name )
			foreach ( player in level.players )
				if ( player IsTouching( trigger ) )
					leader = player;
	maps\_mw2coop::section_reached( start_name, leader, true );
}
