// mw2coop: campaign co-op framework for Call of Duty: Modern Warfare 2 (2009).
// Our own code, loaded through mw2coop's script loader.
//
// Co-op runs in LOCKSTEP: host and client both run every level script. Everything here must
// give the same result on both machines from the same game state. Never branch on anything
// local (local-only dvars, files, wall-clock time) when that changes game state.
//
// Hooks into the game (see mw2coop/patches):
//   maps\_load::main()                     -> main()
//   maps\_utility::is_specialop()          -> false in a co-op campaign session
//   maps\_utility::missionFailedWrapper()  -> mission_failed()
//   maps\_utility::nextmission()           -> mission_complete()
// Requests to the host's mw2coop DLL go through the dvar "mw2coop_cmd" (see request()).

#include common_scripts\utility;
#include maps\_utility;
#include maps\_hud_util;

// A co-op session on a campaign map: the campaign, played by two.
is_campaign_coop()
{
	if ( GetDvar( "coop" ) != "1" )
		return false;
	return !string_starts_with( ToLower( GetDvar( "mapname" ) ), "so_" );
}

// Level scripts call these before maps\_load::main().
//   flag_name: progress flag of the level; start_name: the level's start point that resumes there.
add_checkpoint( flag_name, start_name )
{
	if ( !isdefined( level.mw2coop_checkpoints ) )
		level.mw2coop_checkpoints = [];
	cp = SpawnStruct();
	cp.flag = flag_name;
	cp.start = start_name;
	level.mw2coop_checkpoints[ level.mw2coop_checkpoints.size ] = cp;
}

split_view_gate()
{
	// The picture is split at once, while the mission's title is still up: its black screen covers both halves and its lines are drawn once over the whole picture as in single
	// player (maps\_introscreen patch: the lines' sort marks them for the native split screen), so the change cannot be seen. A slam zoom from the sky is player 1's view alone:
	// there one picture is kept until it has landed (the intro's flag, at most 45 s).
	wait 0.25;
	if ( flying_intro_level() )
	{
		if ( flag_exist( "introscreen_complete" ) && !flag( "introscreen_complete" ) )
			level waittill_notify_or_timeout( "introscreen_complete", 45 );
		for ( t = 0; t < 15 && isdefined( level.mw2coop_intro_black ) && level.mw2coop_intro_black; t += 0.1 )
			wait 0.1;
		wait 0.3;
	}
	SetDvar( "mw2coop_split_view", "1" );
}

// the levels of maps\_introscreen::flying_intro()
flying_intro_level()
{
	switch ( level.script )
	{
		case "killhouse":
		case "cliffhanger":
		case "estate":
		case "boneyard":
			return true;
		case "roadkill":
			return !getdvarint( "newintro" );
	}
	return false;
}

// maps\_introscreen patch: the mission title's black screen is up (true) or gone (false)
intro_black( on )
{
	level.mw2coop_intro_black = on;
}

intro_black_off_after( time )
{
	wait time;
	level.mw2coop_intro_black = false;
}

// Called from maps\_load::main() right after maps\_coop::main().
main()
{
	// The player a one-player scene plays on: scenes the mod hands to either player (the knife
	// fight) read level.mw2coop_lead where they read level.player. Single player too.
	level.mw2coop_lead = level.player;

	// a scene hid the partner's name (maps\_mw2coop_knife::lead_decided()): show it again
	SetDvar( "mw2coop_hide_names", "" );

	// The objectives' markers and distances on screen ("objectiveHide": a saved dvar, so it outlives the level). A slow-motion breach hides them for its slow motion and
	// shows them again only at the very end of its own thread; in co-op that thread can end early, and the markers stayed hidden for the rest of the level and the
	// next one (owner, 2026-11-16: no distance markers in Endgame after Just Like Old Times). Every level starts with them shown.
	if ( is_campaign_coop() )
		SetSavedDvar( "objectiveHide", false );

	// native split screen (src/native.c): both views are shown only once the mission's intro is over; before that (briefing, title card) there is one picture
	SetDvar( "mw2coop_split_view", "0" );
	thread split_view_gate();

	// the console game's revive HUD sits at 75 in split screen and at 120 otherwise (maps\_coop); issplitscreen() is false in the native split screen
	if ( getdvarint( "mw2coop_native_split" ) )
		level.revive_hud_base_offset = 75;

	// the debug tools of the developer console (dbg_ents, dbg_players, dbg_flags, dbg_ai) and the gameplay options, in any session
	// for the Discord presence (src/discord.c)
	if ( is_campaign_coop() )
		SetDvar( "mw2coop_mode", "coop" );
	else
		SetDvar( "mw2coop_mode", "solo" );
	thread maps\_mw2coop_dbg::start();

	// QA of a level in single player (tools/qa_native.sh with MW2COOP_PLAIN=1): the climb scenario runs without a co-op session
	if ( GetDvar( "mw2coop_qa" ) == "climbsolo" && !is_campaign_coop() )
		thread maps\_mw2coop_qa::climbsolo();
	// QA of a Special Ops op in the split screen (Big Brother's gunner)
	if ( GetDvar( "mw2coop_qa" ) == "bbcheck" && !is_campaign_coop() )
		thread maps\_mw2coop_qa::bbcheck();
	// QA of the Special Ops snowmobile races' start (both players held until "GO")
	if ( GetDvar( "mw2coop_qa" ) == "racestart" && !is_campaign_coop() )
		thread maps\_mw2coop_qa::racestart();
	// a Special Ops op in the native split screen: the watch of sync_weapons for player 2's empty hands (his client keeps its own weapon choice where the server's weapon
	// switch does not reach it: the 32-bit build; the 64-bit DLL delivers it, src/native.c p2_sw_flush). Vlad 2026-11-21, so_ac130_co_hunted.
	if ( getdvarint( "mw2coop_native_split" ) && !is_campaign_coop() && is_specialop() )
		thread specops_p2_hands();

	// the co-op markers (maps\_mw2coop_ping.gsc; the key: src/ping.c), in any co-op session: the campaign and Special Ops, network or split screen
	if ( GetDvar( "coop" ) == "1" )
	{
		maps\_mw2coop_ping::ping_precache();	// (the wheel's texts: before any wait)
		maps\_mw2coop_partner::partner_precache();
		thread maps\_mw2coop_ping::init();
		thread maps\_mw2coop_partner::init(); // the partner's distance by the game's edge marker of him, and "down" while he is
	}
	if ( GetDvar( "mw2coop_qa" ) == "hintkeys" )
		maps\_mw2coop_qa::hintkeys_precache();	// (QA: its texts, before any wait)

	if ( !is_campaign_coop() )
		return;

	level.mw2coop = SpawnStruct();
	level.mw2coop.failing = false;
	level.mw2coop.completing = false;
	level.mw2coop.checkpoint = -1;
	level.mw2coop.section = -1;	// index in level.start_functions of the furthest section reached
	level.mw2coop.started = GetTime();	// the level's first frame (began_with_level)
	if ( !isdefined( level.mw2coop_weapon_sync ) )
		level.mw2coop_weapon_sync = "leader";	// "leader": player 1's changes go to player 2; "both": gains go both ways; "own": each player takes his own (only the start's weapons are copied)
	if ( !isdefined( level.mw2coop_own_weapons ) )
		level.mw2coop_own_weapons = [];	// see own_weapon_busy()

	PrecacheString( &"SCRIPT_CHECKPOINT" );
	PrecacheString( &"MW2COOP_STUNNED" );	// the mod's own, from its DLL (src/localize.c)
	PrecacheString( &"MW2COOP_SWAP_SEATS" );

	// maps\_coop (revive) and kill_wrapper() read this Spec Ops flag in any co-op session;
	// maps\_specialops creates it, the campaign does not. Set when the level ends, as in Spec Ops,
	// so the revive HUD and downed-player threads stop.
	if ( !flag_exist( "special_op_terminated" ) )
		flag_init( "special_op_terminated" );

	state( "level " + level.script + ", start dvar '" + GetDvar( "start" ) + "', qa '" + GetDvar( "mw2coop_qa" ) + "'" );

	thread no_saves();
	fov_pilot( undefined );	// a level that ended in a pilot's view must not leave it on
	friendly_fire_setting();
	flag_init( "mw2coop_regrouped" );
	thread regroup_after_start();
	thread follow_leader_teleports();
	// These are QA diagnostics, not gameplay systems. The recorder polls both players' buttons
	// and movement state every 50 ms and writes a detailed session trace; the camera probe feeds
	// the native camera-mismatch logger. Enable both before loading a mission with
	// `set mw2coop_diagnostics 1`; normal play does not create either thread.
	if ( GetDvar( "mw2coop_diagnostics" ) == "1" )
	{
		thread camera_probe();
		thread maps\_mw2coop_rec::start();	// what both players do and where they are, into mw2coop.log
	}
	set_bodies();
	thread sync_weapons();
	thread watch_false_falls();
	thread track_checkpoints();
	thread maps\_mw2coop_checkpoints::watch_flags();
	thread watch_failure();
	if ( god_mode_on() )
		thread god_mode();

	if ( GetDvar( "mw2coop_qa" ) != "" )
	{
		thread maps\_mw2coop_qa::link_probe();
		thread maps\_mw2coop_qa::museum_anim_probe();
		thread maps\_mw2coop_qa::main();
	}
}

// --------------------------------------------------------------------------------------------
// Third-person bodies: campaign players have no model, so the partner was invisible, or only the
// first-person arms of a scene rig showed where he stood. Each player gets the Spec Ops co-op
// body of the level's uniform (maps\_mw2coop_bodies; the DLL loads the zone that holds it);
// _loadout's UpdateModel, as in Spec Ops, sets it and keeps the current weapon's model attached.
// A player never sees his own body. Precached here, before the level's first wait.

set_bodies()
{
	SetDvar( "mw2coop_transplant", maps\_mw2coop_bodies::transplant_request( level.script ) );
	foreach ( n, player in level.players )
	{
		models = maps\_mw2coop_bodies::player_bodies( level.script, n );
		if ( !isdefined( models ) )
			return;
		PrecacheModel( models[ 0 ] );
		PrecacheModel( models[ 1 ] );
		if ( level.script == "cliffhanger" )
		{
			PrecacheModel( maps\_mw2coop_bodies::rider_model( models[ 0 ] ) );
			PrecacheModel( "mw2coop_pick_up" );
		}
		if ( knife_fight_level() )	// the knife in the bodies' left hands (attach_weapon_model; src/transplant.c turned_copy)
			PrecacheModel( "mw2coop_knife_held" );
		if ( knife_fight_level() && GetDvar( "mw2coop_qa" ) == "knifehand" )
			PrecacheModel( "mw2coop_knife_reverse" );
		if ( level.script == "af_caves" || level.script == "gulag" )	// the stand-in on the rope (maps\_mw2coop_cave::partner_view, maps\_mw2coop_gulag::rope_partner_view)
			PrecacheModel( maps\_mw2coop_bodies::rider_model( models[ 0 ], maps\_mw2coop_bodies::rope_skeleton() ) );
		player.mw2coop_body = models[ 0 ];
		player.mw2coop_head = models[ 1 ];
		player maps\_mw2coop_bodies::give_body( ::body_model );
	}
}

// The levels with Endgame's knife fight: af_chase in the campaign (the boat chase, the crash, the wake-up and the fight) and ending, whose zone
// carries the same scripts (the QA scenarios of the knife ran there). Until 2026-11-18 only ending was named: in the campaign the partner held
// ending_knife's world model, a silenced pistol (owner's screenshot of the wake-up in split screen: "where is the knife fix?").
knife_fight_level()
{
	return level.script == "af_chase" || level.script == "ending";
}

// maps\_loadout::UpdateModel, for the current weapon's model (in the default case, attached to the
// right hand). Cliffhanger's ice picks: in the pistol grip the pick stuck out ahead of the fist
// like a pistol (owner: "turn them upright, right into his hands"): the DLL's upright copy of the
// pick (mw2coop_pick_up, src/transplant.c upright_pick), one in each hand.
attach_weapon_model( weapon_model )
{
	if ( weapon_model == "weapon_ice_picker" && is_campaign_coop() && level.script == "cliffhanger" )
	{
		self attach( "mw2coop_pick_up", "tag_weapon_right", true );
		self attach( "mw2coop_pick_up", "tag_weapon_left", true );
		return;
	}
	// Endgame's knife (ending_knife) is never seen in third person in single player: its world model is a silenced pistol, and the
	// partner saw player 1 walk to Shepherd with a gun (owner, 2026-11-14). The players' knife of that scene is shown instead, in the
	// left hand as the player sees it in first person: on tag_weapon_left, where the level itself puts it on the player's rig in the
	// fight (af_chase_knife_fight.gsc: player_rig Attach( "weapon_commando_knife", "TAG_WEAPON_LEFT" )); owner, 2026-11-18: "the knife
	// should lie in the left hand, as in the first-person view"). On a co-op body that tag is not on the hand (the knife hung ahead of
	// it), so the DLL's copy mw2coop_knife_held carries the rig's hold (the knife's place and turn from the rig's left wrist, the same in
	// every frame of the fight's animation) and goes on the body's left wrist. The body has no pose of its own for a held knife
	// (mp/playeranim.script: only the throwing knife's pull-out and throw); docs/32bit-native-map/weapon-animtype.md.
	if ( is_campaign_coop() && knife_fight_level() && weapon_model == GetWeaponModel( "ending_knife" ) )
	{
		self attach( "mw2coop_knife_held", "j_wrist_le", true );
		return;
	}
	self attach( weapon_model, "tag_weapon_right", true );
}

// The left hand's copy of a dual-wield weapon: none for the ice picks, whose upright pair
// attach_weapon_model puts in both hands (a pick ahead of the left fist made four picks, owner).
attach_weapon_model_left( weapon_model )
{
	if ( weapon_model == "weapon_ice_picker" && is_campaign_coop() && level.script == "cliffhanger" )
		return;
	self attach( weapon_model, "tag_weapon_left", true );
}

body_model()
{
	self SetModel( self.mw2coop_body );
	self Attach( self.mw2coop_head, "", true );
}

// --------------------------------------------------------------------------------------------
// God mode for testing: on only when the host sets "mw2coop_god 1" before coopLaunch (the DLL
// sends the value to the client, so both lockstep machines agree; tools/qa_pair.sh sets it).
// Players take no damage, so they are never downed; scripted deaths and mission failures still
// happen. It used to be on unless the dvar was "0", and since the release's .bat launchers were
// replaced by the in-game lobby nothing set "0": every lobby session ran invulnerable.

god_mode_on()
{
	return GetDvar( "mw2coop_god" ) == "1";
}

god_mode()
{
	state( "god mode on (test: mw2coop_god 1)" );
	for ( ;; )
	{
		foreach ( player in level.players )
		{
			if ( !IsAlive( player ) )
				continue;
			player EnableInvulnerability();	// level scripts turn it off at times; put it back every frame
			if ( player.health < player.maxhealth )
				player.health = player.maxhealth;
		}
		wait 0.05;
	}
}

// --------------------------------------------------------------------------------------------
// Messages to the outside: the DLL logs mw2coop_state on both machines and acts on
// mw2coop_cmd on the host only, when its value changes. Every request carries a number that
// keeps counting through restarts and levels (dvar mw2coop_cmd_n): GetTime() alone starts again
// with each restart, and a request sent at the same level time as the last one (a restart from a
// checkpoint, after the same 30 s twice; QA tour 2026-10-22) was the same value and was dropped.
// The number counts on each PC by itself and can differ (a frame the host restarted from before the client ran it): it decides nothing, and the DLL keeps
// GetDvar's read of it out of the games' state hash (src/netwatch.c nw_private_dvar). Never branch on it here.

state( text )
{
	SetDvar( "mw2coop_state", text );
}

// The DLL reads mw2coop_cmd every 250 ms, so requests are queued and sent 0.3 s apart.
request( what )
{
	if ( !isdefined( level.mw2coop_requests ) )
	{
		level.mw2coop_requests = [];
		level thread send_requests();
	}
	level.mw2coop_requests[ level.mw2coop_requests.size ] = what;
	level notify( "mw2coop_request" );
}

send_requests()
{
	for ( ;; )
	{
		while ( level.mw2coop_requests.size == 0 )
			level waittill( "mw2coop_request" );
		what = level.mw2coop_requests[ 0 ];
		level.mw2coop_requests = array_remove_index( level.mw2coop_requests, 0 );
		n = GetDvarInt( "mw2coop_cmd_n" ) + 1;
		SetDvar( "mw2coop_cmd_n", n );
		SetDvar( "mw2coop_cmd", what + " @" + GetTime() + "#" + n );
		wait 0.3;
	}
}

// --------------------------------------------------------------------------------------------
// Saves. The game's own checkpoints (maps\_autosave: SaveGameNoCommit / CommitSave, and the level's start) are taken in a co-op campaign as in single player: each
// machine runs the same scripts in lockstep and keeps the same save in memory (a co-op session writes none to disk). A failed mission loads the last one on both
// (mission_failed, src/saves.c): the whole level as it was, its scripts, AI, objectives and dialogue included. The level start points (checkpoints of
// maps\_mw2coop_checkpoints) stay for "Continue" in a later session. [dvar mw2coop_saves 0: the game's saves off, a failed mission restarts at the start point]

saves_on()
{
	return GetDvar( "mw2coop_saves" ) != "0";
}

no_saves()
{
	if ( saves_on() )
		return;
	waittillframeend;	// maps\_load sets sv_saveOnStartMap after we are called
	SetSavedDvar( "sv_saveOnStartMap", false );
	if ( isdefined( level.flag ) && isdefined( level.flag[ "disable_autosaves" ] ) )
		flag_set( "disable_autosaves" );
}

// --------------------------------------------------------------------------------------------
// Keeping player 2 with player 1

// Start points move player 1 at the end of the first frame. If player 1 is not standing on
// ground yet (a scene, a climb, a vehicle), wait until he is, or until a level script seats
// player 2 somewhere itself.
regroup_after_start()
{
	level endon( "mw2coop_level_end" );
	if ( !isdefined( level.player2 ) )
		return;
	wait 0.1;
	restore_checkpoint_spot();
	thread restore_ally_positions();
	thread restore_weapons();
	thread restore_flags();
	linked_for = 0;
	edge_said = false;
	for ( waited = 0; waited < 120; waited += 0.25 )
	{
		// player 2 seated by the level itself (a vehicle, a scene) stays where it put him; a link of the level's first frames only is waited
		// out: Loose Ends' intro holds both players for half a second at every start point, and at the forest fight's (a checkpoint) player 2
		// was left at the level's spawn, 2100 units behind, until player 1 next moved far (night tour 2026-11-19)
		if ( level.player2 IsLinked() )
		{
			linked_for += 0.25;
			if ( linked_for >= 1 )
				break;
			wait 0.25;
			continue;
		}
		linked_for = 0;
		if ( level.player2 teleport_to_partner( level.player ) )
		{
			state( "regrouped at start point " + level.start_point + ": player 2 from " + vec_text( level.player2.mw2coop_moved_from ) + " to " + vec_text( level.player2.origin ) + ", "
				+ level.player2.mw2coop_moved_how + " (player 1 at " + vec_text( level.player.origin ) + ")" );
			break;
		}
		if ( !edge_said && isdefined( level.player2.mw2coop_moved_how ) && level.player2.mw2coop_moved_how == "edge" )
		{
			edge_said = true;
			state( "regroup: no free spot beside player 1 at an edge (player 1 at " + vec_text( level.player.origin ) + "): player 2 waits at " + vec_text( level.player2.origin ) );
		}
		wait 0.25;
	}
	if ( waited >= 120 )
		state( "no safe spot next to player 1 within 120 s (player 2 at " + vec_text( level.player2.origin ) + ", player 1 at " + vec_text( level.player.origin ) + ")" );
	// a level placing the two players itself waits for this (maps\_mw2coop_trainer)
	flag_set( "mw2coop_regrouped" );
}

// Where player 2's body is, for the split screen's camera watch (native.c: mw2coop_p2pos "x y z linked stance"): if his camera is ever somewhere else the log says so.
camera_probe()
{
	level endon( "mw2coop_level_end" );
	for ( ;; )
	{
		wait 0.5;
		p = level.player2;
		if ( !isdefined( p ) ) // (player 2 may not exist yet when this thread starts: a level start that spawns him late)
			continue;
		linked = 0;
		if ( p IsLinked() )
			linked = 1;
		stance = 0; // GetStance() is a word: stand 0, crouch 1, prone 2
		if ( p GetStance() == "crouch" )
			stance = 1;
		else if ( p GetStance() == "prone" )
			stance = 2;
		SetDvar( "mw2coop_p2pos", p.origin[ 0 ] + " " + p.origin[ 1 ] + " " + p.origin[ 2 ] + " " + linked + " " + stance );
	}
}

// When a script moves player 1 far in one step (teleport, start point), bring player 2 along.
follow_leader_teleports()
{
	level endon( "mw2coop_level_end" );
	if ( !isdefined( level.player2 ) )
		return;
	wait 0.15;
	last = level.player.origin;
	far_time = 0;
	follow = false;
	for ( ;; )
	{
		wait 0.05;
		now = level.player.origin;
		// a seated or otherwise linked player 2 rides along with whatever holds him; a level
		// whose players go separate ways for a while (the Cliffhanger climb lanes) sets
		// level.mw2coop_no_follow
		if ( DistanceSquared( now, last ) > 512 * 512 && !level.player2 IsLinked() && !isdefined( level.mw2coop_no_follow ) )
			follow = true;
		last = now;
		// player 1 may land in a scene or in the air: follow once he stands somewhere
		if ( follow && !level.player2 IsLinked() && level.player2 teleport_to_partner( level.player ) )
		{
			follow = false;
			state( "followed a teleport of player 1" );
		}

		// Leash: a slow scripted move of player 1 (a blended link, a scene) or simply drifting
		// apart leaves player 2 far behind; after a while bring him over. Only while both are
		// free and up; a level can turn it off with level.mw2coop_no_leash.
		if ( leash_applies() && DistanceSquared( now, level.player2.origin ) > leash_distance() * leash_distance() )
			far_time += 0.05;
		else
			far_time = 0;
		// the one behind goes to the one ahead (each player's checkpoint progress, as the checkpoints count it): player 2 was always brought to player 1,
		// also when he was the one far ahead (audit 2026-11-18, GSC F4); with the same progress player 2 goes, as before
		if ( far_time >= leash_time() )
		{
			mover = level.player2;
			target = level.player;
			if ( progress_of( level.player2 ) > progress_of( level.player ) )
			{
				mover = level.player;
				target = level.player2;
			}
			if ( mover teleport_to_partner( target ) )
			{
				far_time = 0;
				if ( mover == level.player2 )
					state( "leash: player 2 brought to player 1" );
				else
					state( "leash: player 1 brought to player 2 (who is further on)" );
			}
		}
	}
}

leash_distance()
{
	return 3000;
}

leash_time()
{
	return 8;	// seconds
}

leash_applies()
{
	if ( isdefined( level.mw2coop_no_leash ) && level.mw2coop_no_leash )
		return false;
	if ( isdefined( level.mw2coop_no_follow ) )
		return false;
	if ( level.player IsLinked() || level.player2 IsLinked() )
		return false;
	return !is_downed( level.player ) && !is_downed( level.player2 );
}

// self: the player to move. Tries free spots beside and behind the partner, each with a clear
// line from him and floor at his height. Moves nobody and returns false while the partner is
// not standing on ground (linked, falling, climbing): his origin may be over a drop, and a
// player put there falls (Cliffhanger). Returns true once moved.
teleport_to_partner( partner )
{
	if ( !IsAlive( partner ) || !is_standing( partner ) )
		return false;
	angles = partner GetPlayerAngles();
	yaw = ( 0, angles[ 1 ], 0 );
	forward = AnglesToForward( yaw );
	right = AnglesToRight( yaw );
	offsets = [];
	offsets[ offsets.size ] = right * 48;
	offsets[ offsets.size ] = right * -48;
	offsets[ offsets.size ] = forward * -48;
	offsets[ offsets.size ] = right * 64 + forward * -48;
	offsets[ offsets.size ] = right * -64 + forward * -48;
	offsets[ offsets.size ] = forward * -96;
	offsets[ offsets.size ] = right * 96;
	offsets[ offsets.size ] = right * -96;
	chest = partner.origin + ( 0, 0, 48 );
	best = undefined;
	best_open = -1;
	drop = false;
	foreach ( offset in offsets )
	{
		spot = partner.origin + offset;
		// a clear line from the partner (ignoring the partner's own body) ...
		if ( !BulletTracePassed( chest, spot + ( 0, 0, 48 ), false, partner ) )
			continue;
		// ... and floor under the spot, at about the partner's height
		ground = PlayerPhysicsTrace( spot + ( 0, 0, 48 ), spot - ( 0, 0, 96 ) );
		if ( ground[ 2 ] < partner.origin[ 2 ] - 40 )
			drop = true;	// open air beside him: an edge
		if ( abs( ground[ 2 ] - partner.origin[ 2 ] ) > 40 )
			continue;
		// ... where he fits: a step is free at his feet (Wolverines! start burgertown put him in
		// a spot open at knee height but stuck, QA tour 2026-10-22) ...
		if ( spot_stuck( ground ) )
			continue;
		// ... with room around it: the S.S.D.D. range start had a free spot 48 units left of
		// player 1 wedged between sandbags and the rifle table, open on 3 sides of 8 (owner's
		// screenshot and QA pickups, 2026-10-21). The first open spot wins, else the most open.
		open = open_sides( ground );
		if ( open >= 5 )
		{
			best = ground;
			break;
		}
		if ( open > best_open )
		{
			best = ground;
			best_open = open;
		}
	}
	self.mw2coop_moved_from = self.origin;
	if ( isdefined( best ) )
	{
		self SetOrigin( best );
		self SetPlayerAngles( yaw );
		self.mw2coop_moved_how = "beside him";
		return true;
	}
	// nowhere free: share the spot he stands on, the players push apart. Not at an edge: the push threw one of them over it (Cliffhanger's cave
	// ledge, entered from Team Player's end: player 2 fell to his death at every start and the mission restarted, owner and network tour 2026-11-21).
	// The caller tries again later, when the partner stands somewhere wider.
	if ( drop )
	{
		self.mw2coop_moved_how = "edge";
		return false;
	}
	self SetOrigin( partner.origin );
	self SetPlayerAngles( yaw );
	self.mw2coop_moved_how = "on his spot";
	return true;
}

// The player an enemy goes for (maps\_spawner, maps\_load: "player seekers", a favourite enemy
// "player"): the closest one alive and up, as Spec Ops does with
// maps\_utility::get_closest_player_healthy() (that one needs the revive flags, which a campaign
// level may not have yet). Player 1 outside co-op, as the original.
seek_player( origin )
{
	if ( !isdefined( level.player2 ) )
		return level.player;
	best = level.player;
	best_d = undefined;
	foreach ( player in level.players )
	{
		if ( !IsAlive( player ) || is_downed( player ) )
			continue;
		d = DistanceSquared( origin, player.origin );
		if ( !isdefined( best_d ) || d < best_d )
		{
			best = player;
			best_d = d;
		}
	}
	return best;
}

// A scene that hides "the player" from enemies (level.player.ignoreme) hides both players: the
// partner is in the same scene (tools/seek_patch.py). Player 1 alone outside co-op.
players_ignoreme( on )
{
	foreach ( player in level.players )
		player.ignoreme = on;
}

// A scene that sets how well enemies hit "the player" (level.player.attackeraccuracy, e.g. 0 while
// he cannot fight back) or how much they want him (level.player.threatbias) sets it on both: the
// partner is in the same scene.
players_attackeraccuracy( value )
{
	foreach ( player in level.players )
		player.attackeraccuracy = value;
}

players_threatbias( value )
{
	foreach ( player in level.players )
		player.threatbias = value;
}

// A level's intro (the mission title on black) belongs to the mission's first start. A co-op
// restart at a checkpoint, or a campaign continued from one, starts the level at that start point,
// and the intro's own skip for other starts is dev-only code (/# #/ in introscreen_delay): in the
// retail game both players saw the title and stood frozen again after every death (QA tour,
// 2026-10-22). In single player a checkpoint is a saved game, without an intro either.
intro_skipped()
{
	return is_campaign_coop() && !is_default_start();
}

// A level's intro (maps\_introscreen: the mission title on black, the slam zoom) holds player 1
// still and without weapons; it holds every player (QA buttons, 2026-10-21: player 2 walked and
// took a pistol while player 1 was held). Player 1 alone outside co-op.
players_freeze_controls( on )
{
	foreach ( player in level.players )
		player FreezeControls( on );
}

players_disable_weapons()
{
	foreach ( player in level.players )
		player DisableWeapons();
}

players_enable_weapons()
{
	foreach ( player in level.players )
		player EnableWeapons();
}

// In how many of 8 directions a player standing at spot could walk 40 units (at knee height).
// True when a player standing at spot could not take an 8-unit step in any of four directions:
// he would be inside something solid.
spot_stuck( spot )
{
	foreach ( yaw in strtok( "0 90 180 270", " " ) )
	{
		to = spot + AnglesToForward( ( 0, int( yaw ), 0 ) ) * 8 + ( 0, 0, 2 );
		if ( Distance( PlayerPhysicsTrace( spot + ( 0, 0, 2 ), to ), to ) < 0.5 )
			return false;
	}
	return true;
}

open_sides( spot )
{
	open = 0;
	from = spot + ( 0, 0, 18 );
	for ( yaw = 0; yaw < 360; yaw += 45 )
	{
		to = from + AnglesToForward( ( 0, yaw, 0 ) ) * 40;
		if ( Distance( PlayerPhysicsTrace( from, to ), to ) < 1 )
			open++;
	}
	return open;
}

// Free and on ground. IsOnGround() reads 0 for player 2 on both machines in co-op (seen in the
// spawn audit), so ground under the feet is traced too.
is_standing( player )
{
	if ( player IsLinked() )
		return false;
	if ( player IsOnGround() )
		return true;
	below = PlayerPhysicsTrace( player.origin + ( 0, 0, 8 ), player.origin - ( 0, 0, 24 ) );
	return player.origin[ 2 ] - below[ 2 ] < 4;
}

// self: the player to move. teleport_to_partner(), retried for up to timeout seconds (the
// partner may still be getting out of a vehicle or a scene).
teleport_to_partner_when_safe( partner, timeout )
{
	for ( waited = 0; waited < timeout; waited += 0.1 )
	{
		if ( self teleport_to_partner( partner ) )
			return true;
		wait 0.1;
	}
	return false;
}

// --------------------------------------------------------------------------------------------
// Weapons: level scripts give and take weapons on level.player only.

is_downed( player )
{
	return player ent_flag_exist( "coop_downed" ) && player ent_flag( "coop_downed" );
}

// Special Ops in the native split screen: player 2 holds nothing while he has a weapon of his own (not flying, not in the AC-130 or the helicopter's gun, not linked, not
// downed): the DLL presses his weapon key (mw2coop_p2_weapnext), then a switch to his first weapon follows, as sync_weapons does in the campaign.
specops_p2_hands()
{
	level endon( "special_op_terminated" );
	empty = 0;
	inv_was = undefined;
	inv_at = 0;
	for ( ;; )
	{
		wait 0.25;
		p = level.player2;
		if ( !isdefined( p ) )
			continue;
		weapons = p GetWeaponsListPrimaries();
		inv = p GetWeaponsListAll();	// (his own pickup: the hands are "none" for about 350 ms between the dropped weapon and the new one; see sync_weapons)
		if ( !isdefined( inv_was ) || !same_weapon_list( inv, inv_was ) )
		{
			inv_was = inv;
			inv_at = GetTime();
		}
		in_air = ( isdefined( level.ac130player ) && level.ac130player == p ) || ( isdefined( level.chopperplayer ) && level.chopperplayer == p );
		if ( p GetCurrentWeapon() == "none" && weapons.size > 0 && !in_air && !p IsLinked() && !is_downed( p ) && !isdefined( p.is_flying_missile )
			&& !( isdefined( p.is_controlling_UAV ) && p.is_controlling_UAV ) && GetTime() - inv_at >= 2000 )
		{
			empty++;
			if ( empty % 12 == 4 )	// (a moment's empty hands are a switch in progress: 1 s, then every 3 s)
			{
				SetDvar( "mw2coop_p2_weapnext", GetTime() );
				state( "Spec Ops: player 2's hands are empty: weapon key pressed for him" );
			}
			else if ( empty % 12 == 8 )
				p SwitchToWeapon( weapons[ 0 ] );
		}
		else
			empty = 0;
	}
}

give_weapon( player, weapon )
{
	player GiveWeapon( weapon );
	player GiveMaxAmmo( weapon );
	if ( player GetCurrentWeapon() == "none" && is_in_array( player GetWeaponsListPrimaries(), weapon ) )
		player SwitchToWeapon( weapon );
}

// Player 2 starts with exactly player 1's weapons.
copy_weapons( to, from )
{
	foreach ( weapon in to GetWeaponsListAll() )
		if ( !from HasWeapon( weapon ) )
			to TakeWeapon( weapon );
	foreach ( weapon in from GetWeaponsListAll() )
		if ( !to HasWeapon( weapon ) )
			give_weapon( to, weapon );
	current = from GetCurrentWeapon();
	if ( current != "none" && to HasWeapon( current ) )
		to SwitchToWeapon( current );
}

// A loadout gives weapons and then switches to one (airport: m4_grenadier, m240, then
// SwitchToWeapon m240); while player 1 raises it his current weapon is still the first one, so
// copy_weapons() put player 2 on the M4 (owner, 2026-10-21: "the client got a rifle instead of
// the machine gun"). Player 2 holds whatever player 1 settles on during the first seconds.
follow_first_weapon_switch()
{
	level endon( "mw2coop_level_end" );
	asked = level.player2 GetCurrentWeapon();
	for ( t = 0; t < 4; t += 0.1 )
	{
		wait 0.1;
		current = level.player GetCurrentWeapon();
		if ( current != "none" && current != asked && level.player2 HasWeapon( current ) && level.player2 GetCurrentWeapon() != current )
		{
			asked = current;
			level.player2 SwitchToWeapon( current );
			state( "player 2 raises " + current + " as player 1 does" );
		}
	}
}

// Level loadouts (maps\_loadout) set first-person hands on level.player only. Without hands the
// client's own player cannot draw a weapon: the engine raises "no viewmodel set" and drops the
// co-op session. Player 2 always wears player 1's hands, also when a level changes them.
sync_viewmodel()
{
	hands = level.player GetViewModel();
	if ( hands != "" && level.player2 GetViewModel() != hands )
		level.player2 SetViewModel( hands );
}

// Weapons each player's own scene takes and gives back (the Cliffhanger ice picks: the climb
// takes them from the climbing player and returns them at the top): never taken from player 2
// because player 1 lost them, and not handed to player 2 while he is in such a scene himself
// (linked). Playtest 2026-10-23: player 1 started climbing and player 2 lost his picks; player 1
// reached the top and player 2, still on the wall, got the picks back and fell.
own_weapon_busy( weapon )
{
	return is_in_array( level.mw2coop_own_weapons, weapon ) && level.player2 IsLinked();
}

sync_weapons()
{
	level endon( "mw2coop_level_end" );
	if ( !isdefined( level.player2 ) )
		return;
	sync_viewmodel();	// before any weapon reaches player 2
	wait 0.15;	// after start points hand out weapons
	sync_viewmodel();
	copy_weapons( level.player2, level.player );
	state( "player 2 weapons copied from player 1" );
	thread follow_first_weapon_switch();
	prev1 = level.player GetWeaponsListAll();
	prev2 = level.player2 GetWeaponsListAll();
	for ( ;; )
	{
		wait 0.25;
		sync_viewmodel();
		if ( isdefined( level.mw2coop_sync_hold ) && level.mw2coop_sync_hold > GetTime() )	// the checkpoint's weapons are being put back (restore_weapons): not a change of player 1's
		{
			prev1 = level.player GetWeaponsListAll();
			prev2 = level.player2 GetWeaponsListAll();
			continue;
		}
		now1 = level.player GetWeaponsListAll();
		now2 = level.player2 GetWeaponsListAll();
		// the revive system takes and returns weapons of a downed player: leave it alone
		// Each player's own inventory (owner, 2026-11-16: "the players' inventories must be separate"): a weapon player 1 picks up from the ground (one weapon in
		// his hands goes, another comes, in the same moment) is his alone, and the one he left there is not taken from player 2. What a level gives or takes
		// (a weapon that comes or goes alone, a whole loadout) still goes to both.
		swapped = false;
		if ( !isdefined( level.mw2coop_shared_inventory ) )
		{
			gained = 0;
			lost = 0;
			foreach ( weapon in now1 )
				if ( !is_in_array( prev1, weapon ) )
					gained++;
			foreach ( weapon in prev1 )
				if ( !is_in_array( now1, weapon ) )
					lost++;
			swapped = gained == 1 && lost == 1;
			if ( swapped )
				state( "player 1 swapped a weapon on the ground: his own (player 2 keeps his)" );
		}
		downed = is_downed( level.player ) || is_downed( level.player2 );
		if ( level.mw2coop_weapon_sync != "own" && !swapped && !downed )
		{
			foreach ( weapon in now1 )
				if ( !is_in_array( prev1, weapon ) && !level.player2 HasWeapon( weapon ) && !own_weapon_busy( weapon ) && !forced_pistol( level.player, weapon ) )
					give_weapon( level.player2, weapon );
			if ( level.mw2coop_weapon_sync == "both" )
				foreach ( weapon in now2 )
					if ( !is_in_array( prev2, weapon ) && !level.player HasWeapon( weapon ) )
						give_weapon( level.player, weapon );
			foreach ( weapon in prev1 )
				if ( !is_in_array( now1, weapon ) && level.player2 HasWeapon( weapon ) && !is_in_array( level.mw2coop_own_weapons, weapon ) )
					level.player2 TakeWeapon( weapon );
		}
		// Player 2 does not stand with empty hands while player 1 holds a weapon (airport: after the elevator player 1 holds the saw and the machine gun, player 2 held nothing until he pressed
		// 1 or Y himself; testers, 2026-11-06). His client keeps its own weapon choice and undoes a script's SwitchToWeapon, so the DLL presses his pad's weapon key (the dvar
		// mw2coop_p2_weapnext, native.c), then the switch to player 1's weapon follows. Only while he is free (not in a scene, not downed).
		// (also when both ride something and player 1 has his weapon there: the Gulag fly-in gives player 1 the scoped M14 to shoot from the helicopter, player 2 sat beside him with
		// empty hands; owner, 2026-11-16: "the bottom player should get a weapon", "not only this level")
		// (not while he flies the Predator: maps\_remotemissile unlinks him and turns his weapons off for the missile's flight, and gives his weapon back itself)
		// (not in the middle of a weapon change of his own: a pickup from the ground leaves his hands "none" for about 350 ms between the weapon he drops and the
		// one he takes, and the key pressed then switched him from the picked weapon to the next one: a player's friend "could not pick weapons up, the game
		// switched them for him", 2026-11-25, QA deadpick3 with network co-op. The hands must stay empty for 1 s, and not within 2 s of a change of his weapons
		// that player 1's did not share: his own pickup. A level's loadout changes both players' weapons in the same moment (the airport after the elevator:
		// the saw goes, the machine gun comes) and is answered after 1 s, QA heldweapons. specops_p2_hands waits too.)
		inv2 = level.player2 GetWeaponsListAll();
		if ( !isdefined( level.mw2coop_p2_inv ) || !same_weapon_list( inv2, level.mw2coop_p2_inv ) )
		{
			if ( isdefined( level.mw2coop_p1_inv ) && same_weapon_list( now1, level.mw2coop_p1_inv ) )
				level.mw2coop_p2_inv_at = GetTime();
			level.mw2coop_p2_inv = inv2;
		}
		level.mw2coop_p1_inv = now1;
		if ( level.player2 GetCurrentWeapon() == "none" && ( !level.player2 IsLinked() || level.player IsLinked() )
			&& ( !isdefined( level.mw2coop_p2_inv_at ) || GetTime() - level.mw2coop_p2_inv_at >= 2000 ) && !is_downed( level.player2 ) && !is_downed( level.player ) && level.player GetCurrentWeapon() != "none" && level.player2 GetWeaponsListPrimaries().size > 0
			&& !isdefined( level.player2.is_flying_missile ) && !( isdefined( level.player2.is_controlling_UAV ) && level.player2.is_controlling_UAV ) )
		{
			if ( !isdefined( level.mw2coop_empty_hands ) )
				level.mw2coop_empty_hands = 0;
			level.mw2coop_empty_hands++;
			held = level.player GetCurrentWeapon();
			if ( level.mw2coop_empty_hands % 12 == 4 )	// 1 s of empty hands, then every 3 s
			{
				SetDvar( "mw2coop_p2_weapnext", GetTime() );
				level.mw2coop_weapnext_presses = int_or_zero( level.mw2coop_weapnext_presses ) + 1;	// (QA: deadpick counts the presses; a level variable, never a dvar each PC counts itself)
				state( "player 2's hands are empty while player 1 holds " + held + ": weapon key pressed for him" );
			}
			else if ( level.mw2coop_empty_hands % 12 == 8 && level.player2 HasWeapon( held ) )
				level.player2 SwitchToWeapon( held );
		}
		else
			level.mw2coop_empty_hands = 0;
		// While someone is down the comparison waits (the revive system's pistol comes and goes meanwhile): what the level gave or took in the
		// meantime is mirrored once both are up. Before, the lists were refreshed anyway and such a change never reached player 2 (night audit
		// 2026-11-17, GSC-3).
		if ( !downed )
		{
			prev1 = level.player GetWeaponsListAll();
			prev2 = level.player2 GetWeaponsListAll();
		}
	}
}

// the same weapons in two lists (any order)
same_weapon_list( a, b )
{
	if ( a.size != b.size )
		return false;
	foreach ( weapon in a )
		if ( !is_in_array( b, weapon ) )
			return false;
	return true;
}

int_or_zero( v )
{
	if ( !isdefined( v ) )
		return 0;
	return v;
}

// The pistol the revive system hands a downed player who has none (maps\_coop player_coop_force_switch_to_pistol), not a weapon of the level's.
forced_pistol( player, weapon )
{
	return isdefined( player.forced_pistol ) && player.forced_pistol == weapon;
}

// --------------------------------------------------------------------------------------------
// Falling damage without a fall. Vlad's Oil Rig session (2026-11-08, log mw2coop(4)): after a restart player 2 stood one unit above the deck, "in the air" and not moving, for
// 20 s; then he "landed" at the same spot with 100 damage from MOD_FALLING and was downed at once ("player 2 hurt himself"). The engine measures the fall from where the
// player left the ground, and that point was stale. A fall of 140 units or more is needed for any damage, and the player must have been that much higher in the seconds before:
// damage of that kind without it is undone (the health goes back to what it was a tick earlier).

watch_false_falls()
{
	level endon( "mw2coop_level_end" );
	foreach ( player in level.players )
		player thread false_fall_guard();
}

false_fall_guard()
{
	level endon( "mw2coop_level_end" );
	self endon( "death" );
	self.mw2coop_zs = [];
	self.mw2coop_h0 = self.health;
	thread false_fall_sampler();
	for ( ;; )
	{
		self waittill( "damage", amount, attacker, direction, point, type );
		if ( !isdefined( type ) || type != "MOD_FALLING" || is_downed( self ) )
			continue;
		if ( isdefined( level.mw2coop_fall_guard_off ) && level.mw2coop_fall_guard_off )	// QA: to see what the engine does without the guard
			continue;
		height = fall_height( self );
		if ( height >= 64 )
			continue;
		if ( self.mw2coop_h0 > self.health && self.mw2coop_h0 <= self.maxhealth )
			self.health = self.mw2coop_h0;
		state( "falling damage " + amount + " without a fall (" + height + " units) undone, health " + self.health );
	}
}

// How far the player has dropped in the last 3 seconds (0 when he has not).
fall_height( player )
{
	top = player.origin[ 2 ];
	if ( isdefined( player.mw2coop_zs ) )
		foreach ( z in player.mw2coop_zs )
			if ( z > top )
				top = z;
	return top - player.origin[ 2 ];
}

// maps{bs}_coop::player_coop_proc(): a hit that would down the player. True for falling damage without a fall behind it (see watch_false_falls); that hit does not down him.
false_fall_hit( player, type )
{
	if ( !is_campaign_coop() || !isdefined( type ) || type != "MOD_FALLING" )
		return false;
	if ( isdefined( level.mw2coop_fall_guard_off ) && level.mw2coop_fall_guard_off )
		return false;
	return fall_height( player ) < 64;
}

// self: a player. Health of the last tick (the damage notify comes after the health dropped) and the heights of the last 3 s.
false_fall_sampler()
{
	level endon( "mw2coop_level_end" );
	self endon( "death" );
	n = 0;
	k = 0;
	for ( ;; )
	{
		wait 0.05;
		self.mw2coop_h0 = self.health;
		n++;
		if ( n % 2 == 0 )
		{
			self.mw2coop_zs[ k ] = self.origin[ 2 ];
			k = ( k + 1 ) % 30;
		}
	}
}

// --------------------------------------------------------------------------------------------
// Checkpoints: the furthest progress flag reached sets the "start" dvar, so a restart resumes
// from that start point on both machines (the dvar is set by this script, so it matches).

track_checkpoints()
{
	if ( !isdefined( level.mw2coop_checkpoints ) )
		return;
	wait 0.1;	// level.start_point is chosen at the end of the first frame
	foreach ( index, cp in level.mw2coop_checkpoints )
		if ( isdefined( level.start_point ) && level.start_point == cp.start )
			level.mw2coop.checkpoint = index;
	foreach ( index, cp in level.mw2coop_checkpoints )
		thread checkpoint_wait( index, cp );
}

checkpoint_wait( index, cp )
{
	level endon( "mw2coop_level_end" );
	while ( !isdefined( level.flag[ cp.flag ] ) )
		wait 0.05;
	flag_wait( cp.flag );
	if ( index <= level.mw2coop.checkpoint || level.mw2coop.failing )
		return;
	if ( just_started_at_checkpoint() )
		return;
	level.mw2coop.checkpoint = index;
	SetDvar( "start", cp.start );
	state( "checkpoint " + cp.start );
	snapshot_allies();
	snapshot_weapons( cp.start );
	request( "checkpoint " + level.script + " " + cp.start );	// the host saves it for mw2coop_continue
	if ( !saves_on() )	// (with the game's saves its own checkpoints are the ones a failed mission goes back to)
		foreach ( player in level.players )
			player thread checkpoint_hud();
}

// True during the first seconds of a level that was started at a checkpoint (a restart after a failed mission, or a continued game). The start function sets the flags of the sections
// before it, and the level's logic functions of the sections it skips return at once: the checkpoints they would mark are not reached by anybody. Owner's logs, 2026-11-05: a restart
// at "massacre" took "gate" 0.45 s later, one at "solorun_rooftops" took "solorun_chopper" 1.3 s later, and the next death restarted there: on a checkpoint the players had never
// reached. A level that began at its default start is not affected.
just_started_at_checkpoint()
{
	return isdefined( level.start_point ) && level.start_point != "default" && GetTime() < 8000;
}

// The first 3 s of the level, whatever start it began at: no player reaches a section that soon.
began_with_level()
{
	return isdefined( level.mw2coop.started ) && GetTime() - level.mw2coop.started < 3000;
}

// A start point's index in level.start_functions (the last entry of that name, as level.start_arrays keeps), undefined when the level has none of that name.
section_index( name )
{
	index = undefined;
	foreach ( i, start in level.start_functions )
		if ( start[ "name" ] == name )
			index = i;
	return index;
}

// The index of the start point the level began at; -1 for the default start or before it is chosen.
start_section_index()
{
	if ( !isdefined( level.start_point ) )
		return -1;
	index = section_index( level.start_point );
	if ( !isdefined( index ) )
		return -1;
	return index;
}

// Checkpoints for every level: a section of the level begins (the start point "name": its section
// function in maps\_load::handle_starts, or the level's own section handler, patched): it becomes
// the co-op checkpoint, so a failed mission restarts there instead of at the level's beginning, as
// a single player autosave would let him go on. Only forward, only real start points (not the
// developers' test starts, start_is_dev), and not the start point the level began at.
// leader: the player who got there (a checkpoint flag's trigger he touched); undefined when not
// known. by_table: true when a flag or a place of maps\_mw2coop_checkpoints marked it, undefined when the section began (maps\_load::handle_starts,
// a level's section handler).
section_reached( name, leader, by_table )
{
	if ( !is_campaign_coop() || !isdefined( level.mw2coop ) || level.mw2coop.failing || level.mw2coop.completing )
		return;
	name = ToLower( name );	// as maps\_utility::add_start keeps the names
	// a section that begins before the players get there is marked by its flag or place only (maps\_mw2coop_checkpoints::late_sections)
	if ( !isdefined( by_table ) && is_in_array( maps\_mw2coop_checkpoints::late_sections( level.script ), name ) )
	{
		state( "checkpoint " + name + ": its section began, its flag or place will mark it" );
		return;
	}
	index = section_index( name );
	// only forward, and not the start point the level began at or one before it (a start point without a section function of its own is not entered by handle_starts)
	if ( !isdefined( index ) || index <= level.mw2coop.section || index <= start_section_index() )
		return;
	// A section entered with the level is not one anybody reached, and it does not count as passed (a later flag of the level may still mark it): handle_starts() enters the first
	// section whose function waits as soon as the ones before it return, and in Endgame the boat sections' functions are empty (::empty), so the last one, boatdrive_end, was
	// entered in the level's first frame and every failed boat ride restarted at the end of the chase (issue #16; QA cptour log: "checkpoint boatdrive_end (section 6)"
	// 0.04 s after the start). Roadkill: riverbank's function only starts threads, so "convoy" (the foot of the stairs up to the roof fight) was taken at once and a death
	// at the riverbank restarted past the opening scene. A restart at a checkpoint skips sections the same way (just_started_at_checkpoint).
	if ( began_with_level() )
	{
		state( "checkpoint " + name + " not taken: its section began " + ( GetTime() - level.mw2coop.started ) + " ms after the level's start (the sections before it return at once)" );
		return;
	}
	if ( just_started_at_checkpoint() )
	{
		state( "checkpoint " + name + " not taken: " + GetTime() + " ms after the start at " + level.start_point + " (the start function's own flags and the sections it skips)" );
		return;
	}
	level.mw2coop.section = index;
	if ( start_is_dev( name ) )
		return;
	rank = note_checkpoint_rank();
	SetDvar( "start", name );
	state( "checkpoint " + name + " (section " + index + ")" );
	if ( !isdefined( leader ) )
		leader = standing_leader();
	spot = checkpoint_spot( leader );
	SetDvar( "mw2coop_cp_start", name );
	SetDvar( "mw2coop_cp_pos", spot );
	snapshot_allies();
	snapshot_weapons( name );
	snapshot_flags( name );
	request( "checkpoint " + level.script + " " + name + " " + spot );	// the host saves it for mw2coop_continue
	if ( !saves_on() )
		foreach ( player in level.players )
			player thread checkpoint_hud();
	if ( isdefined( leader ) )
	{
		progress_of( leader );
		leader.mw2coop_progress = rank;
	}
	level.mw2coop.cp_spots[ rank ] = checkpoint_spot_origin( leader );
	bring_partner_to( leader, rank );
}

// --------------------------------------------------------------------------------------------
// The weapons and their ammo when a checkpoint was reached. A restart at a checkpoint runs the level's start function, which hands out that start's loadout with full ammunition (owner's
// report, 2026-11-14: after a restart both players' weapons were the level's default loadout again, with full ammo, not what they had at the checkpoint). Like a game's own autosave, the
// checkpoint keeps them: dvars mw2coop_cp_w0 (player 1) and mw2coop_cp_w1 (player 2) hold "current;weapon:clip:stock;..." and mw2coop_cp_wname the checkpoint they belong to; a few
// seconds after a restart at that checkpoint the loadout is replaced by them (restore_weapons). Weapons a player does not have at the checkpoint are taken, the ones he has come back
// with the ammunition he had. Grenades are weapons too (GetWeaponsListAll lists them with their count in the ammo).
snapshot_weapons( name )
{
	players = [];
	players[ 0 ] = level.player;
	players[ 1 ] = level.player2;
	// (the level is part of the name: start points of the same name are in several levels, and a loadout of another level was given back there, weapons the level never
	// loaded among them: audit 2026-11-18, GSC F1)
	SetDvar( "mw2coop_cp_wname", level.script + " " + name );
	foreach ( n, player in players )
	{
		text = "";
		if ( isdefined( player ) && IsAlive( player ) )
		{
			text = player GetCurrentWeapon();
			foreach ( weapon in player GetWeaponsListAll() )
				text += ";" + weapon + ":" + player GetWeaponAmmoClip( weapon ) + ":" + player GetWeaponAmmoStock( weapon );
		}
		SetDvar( "mw2coop_cp_w" + n, text );
	}
}

// The level's flags at a checkpoint. A restart at a checkpoint runs the level's start function for it (a start point made for the developers), which sets the flags of
// its own section only; the level's threads that run through the whole mission (objectives, dialogue, music, the next phase of a scene) wait on flags that a normal run
// set long before, and stood still: the next phase did not come, a line was not said, an objective marker stayed where it had been (owner, 2026-11-17, Loose Ends:
// after restarts at "escape" the marker stayed at the house: estate.gsc objectives() stopped at flag_wait( "deploy_house_defense_jeeps" ) on objective 1, at the house).
// At the checkpoint the names of every set flag are kept (dvars mw2coop_cp_f0.. of 800 characters, mw2coop_cp_fn their count, mw2coop_cp_fname level and start point);
// after the restart at that checkpoint the ones still clear are set again, as they were. [dvar mw2coop_cpflags 0: off]
snapshot_flags( name )
{
	chunks = [];
	text = "";
	count = 0;
	if ( isdefined( level.flag ) )
	{
		foreach ( flag_name, value in level.flag )
		{
			if ( !isdefined( value ) || !value )
				continue;
			if ( text.size + flag_name.size + 1 > 800 )
			{
				chunks[ chunks.size ] = text;
				text = "";
			}
			text += flag_name + " ";
			count++;
		}
	}
	if ( text != "" )
		chunks[ chunks.size ] = text;
	foreach ( i, chunk in chunks )
		SetDvar( "mw2coop_cp_f" + i, chunk );
	SetDvar( "mw2coop_cp_fn", chunks.size );
	SetDvar( "mw2coop_cp_fname", level.script + " " + name );
	level.mw2coop_cp_flags_kept = count;
}

restore_flags()
{
	level endon( "mw2coop_level_end" );
	if ( GetDvar( "mw2coop_cpflags" ) == "0" || !isdefined( level.start_point ) || level.start_point == "default" || GetDvar( "mw2coop_cp_fname" ) != level.script + " " + level.start_point )
		return;
	wait 0.5;	// after the start function (its spawns and its own flags)
	set = 0;
	kept = 0;
	n = GetDvarInt( "mw2coop_cp_fn" );
	for ( i = 0; i < n; i++ )
	{
		foreach ( flag_name in StrTok( GetDvar( "mw2coop_cp_f" + i ), " " ) )
		{
			kept++;
			if ( !isdefined( level.flag[ flag_name ] ) || level.flag[ flag_name ] )
				continue;
			flag_set( flag_name );
			set++;
		}
	}
	state( "restart at " + level.start_point + ": " + set + " of the " + kept + " flags set at the checkpoint were clear and are set again" );
}

restore_weapons()
{
	level endon( "mw2coop_level_end" );
	if ( !isdefined( level.player2 ) || !isdefined( level.start_point ) || level.start_point == "default" || GetDvar( "mw2coop_cp_wname" ) != level.script + " " + level.start_point )
		return;
	wait 1;	// after the start's loadout and the first copy of sync_weapons
	level.mw2coop_sync_hold = GetTime() + 1500;
	players = [];
	players[ 0 ] = level.player;
	players[ 1 ] = level.player2;
	restored = 0;
	foreach ( n, player in players )
	{
		text = GetDvar( "mw2coop_cp_w" + n );
		parts = StrTok( text, ";" );
		if ( parts.size < 2 || is_downed( player ) )
			continue;
		keep = [];
		for ( i = 1; i < parts.size; i++ )
		{
			fields = StrTok( parts[ i ], ":" );
			if ( fields.size == 3 )
				keep[ keep.size ] = fields;
		}
		foreach ( weapon in player GetWeaponsListAll() )
		{
			found = false;
			foreach ( fields in keep )
				if ( fields[ 0 ] == weapon )
					found = true;
			if ( !found )
				player TakeWeapon( weapon );
		}
		foreach ( fields in keep )
		{
			if ( !player HasWeapon( fields[ 0 ] ) )
				player GiveWeapon( fields[ 0 ] );
			player SetWeaponAmmoClip( fields[ 0 ], int( fields[ 1 ] ) );
			player SetWeaponAmmoStock( fields[ 0 ], int( fields[ 2 ] ) );
		}
		if ( parts[ 0 ] != "none" && player HasWeapon( parts[ 0 ] ) )
			player SwitchToWeapon( parts[ 0 ] );
		restored++;
	}
	level.mw2coop_sync_hold = GetTime() + 1500;
	state( "the weapons and ammunition of checkpoint " + level.start_point + " are back for " + restored + " player(s)" );
}

// --------------------------------------------------------------------------------------------
// Where the allies stood when a checkpoint was reached. A restart at a checkpoint runs the level's start function for it, which places only the people it names and leaves the rest where
// the level began (Wolverines!, diner start: the squad, the president and the defenders 4400 units behind the players, the players waiting for them; owner's log, 2026-11-05). A game's own
// autosave would bring everybody back as they were: the allies' places are kept with the checkpoint (dvar mw2coop_cp_allies: "class|name|x_y_z_yaw;..."), and the ones the start left more
// than 600 units from theirs are put back a few seconds after the start. Allies that are gone or linked (a vehicle) are not touched.
ally_name( ai )
{
	name = "-";
	if ( isdefined( ai.script_friendname ) )
		name = ai.script_friendname;
	else if ( isdefined( ai.animname ) )
		name = ai.animname;
	return ai.classname + "|" + name;
}

snapshot_allies()
{
	text = "";
	n = 0;
	foreach ( ai in GetAIArray( "allies" ) )
	{
		if ( !IsAlive( ai ) || n >= 14 )
			continue;
		if ( n > 0 )
			text += ";";
		angles = ai.angles;
		text += ally_name( ai ) + "|" + spot_int( ai.origin[ 0 ] ) + "_" + spot_int( ai.origin[ 1 ] ) + "_" + spot_int( ai.origin[ 2 ] ) + "_" + spot_int( angles[ 1 ] );
		n++;
	}
	// (the level in front: the same start name in another level is not this checkpoint, audit 2026-11-18, GSC F1)
	SetDvar( "mw2coop_cp_allies", level.script + "@" + text );
}

restore_ally_positions()
{
	level endon( "mw2coop_level_end" );
	if ( !isdefined( level.start_point ) || level.start_point == "default" || GetDvar( "mw2coop_cp_start" ) != level.start_point )
		return;
	text = GetDvar( "mw2coop_cp_allies" );
	pre = level.script + "@";
	if ( text.size < pre.size || GetSubStr( text, 0, pre.size ) != pre )
		return;
	text = GetSubStr( text, pre.size, text.size );
	wait 3.5;
	used = [];
	moved = 0;
	entries = [];
	if ( text != "" )
		entries = StrTok( text, ";" );
	foreach ( entry in entries )
	{
		parts = StrTok( entry, "|" );
		if ( parts.size != 3 )
			continue;
		pos = StrTok( parts[ 2 ], "_" );
		if ( pos.size != 4 )
			continue;
		target = ( spot_value( pos[ 0 ] ), spot_value( pos[ 1 ] ), spot_value( pos[ 2 ] ) );
		best = undefined;
		foreach ( ai in GetAIArray( "allies" ) )
		{
			if ( !IsAlive( ai ) || isdefined( used[ "" + ai GetEntityNumber() ] ) || ally_name( ai ) != parts[ 0 ] + "|" + parts[ 1 ] )
				continue;
			if ( !isdefined( best ) || DistanceSquared( ai.origin, target ) < DistanceSquared( best.origin, target ) )
				best = ai;
		}
		if ( !isdefined( best ) )
			continue;
		used[ "" + best GetEntityNumber() ] = true;
		// (an ally the start seated in a vehicle or linked to something stays there, as the comment above says: only IsAlive was checked, audit
		// 2026-11-18, GSC F5)
		if ( isdefined( best.vehicle ) || best IsLinked() )
			continue;
		if ( Distance( best.origin, target ) > 600 )
		{
			best ForceTeleport( target, ( 0, spot_value( pos[ 3 ] ), 0 ) );
			moved++;
		}
	}
	state( "allies put back at the checkpoint " + level.start_point + ": " + moved + " of " + entries.size + " moved" );
	gather_far_allies();
}

// Allies that are still far from both players after a restart at a checkpoint: the start function placed the players at the checkpoint's spot and left the others where the level began, or the
// checkpoint had recorded them already far behind (owner's logs of 2026-11-04: Wolverines! diner, vip_escort, Contingency ridge and base, Second Sun, Of One's Own Free Will: squads 3500 to
// 9000 units from the players for minutes). Each is put on a path node near the nearest player, as a game's own autosave would have him there. 2500 units: squads spread out 1500 at most.
// Allies in a vehicle are left alone. QA: allyfar.
gather_far_allies()
{
	far = 0;
	near = 0;
	riding = 0;
	text = "";
	foreach ( ai in GetAIArray( "allies" ) )
	{
		if ( !IsAlive( ai ) )
			continue;
		if ( isdefined( ai.vehicle ) )
		{
			riding++;
			continue;
		}
		anchor = seek_player( ai.origin );
		if ( Distance( ai.origin, anchor.origin ) < 2500 )
			continue;
		far++;
		nodes = GetNodesInRadius( anchor.origin, 500, 120, 200, "path" );
		if ( nodes.size == 0 )
		{
			text += " " + ally_name( ai ) + " (no node)";
			continue;
		}
		node = nodes[ RandomInt( nodes.size ) ];
		ai ForceTeleport( node.origin, ( 0, node.angles[ 1 ], 0 ) );
		near++;
	}
	state( "allies far from the players after the restart: " + far + ", put near them: " + near + ", in a vehicle: " + riding + text );
}

// --------------------------------------------------------------------------------------------
// Where a checkpoint was reached. Some start points do not place the player (Cliffhanger "c4": its
// start function's teleport is commented out), so a restart there left both players at the level's
// first spawn (playtest 2026-10-23: restart at c4 put them back at the cave). The spot of the player
// who got there is kept with the checkpoint (dvars mw2coop_cp_start, mw2coop_cp_pos, and the host's
// save) and used when the start point leaves the player where the level spawns him.
// Encoded as a plain name for the start packet: x_y_z_yaw, "m" for minus ("m4131_m25410_998_61").

checkpoint_spot( leader )
{
	player = leader;
	if ( !isdefined( player ) || !is_standing( player ) )
	{
		player = undefined;
		foreach ( p in level.players )
			if ( !isdefined( player ) && is_standing( p ) )
				player = p;
	}
	if ( !isdefined( player ) )
		return "";
	angles = player GetPlayerAngles();
	return spot_int( player.origin[ 0 ] ) + "_" + spot_int( player.origin[ 1 ] ) + "_" + spot_int( player.origin[ 2 ] ) + "_" + spot_int( angles[ 1 ] );
}

spot_int( value )
{
	value = int( value );
	if ( value < 0 )
		return "m" + ( 0 - value );
	return "" + value;
}

spot_value( text )
{
	if ( GetSubStr( text, 0, 1 ) == "m" )
		return 0 - int( GetSubStr( text, 1 ) );
	return int( text );
}

// At the start: player 1 to the checkpoint's spot when the start point left him at the level's
// spawn. Both machines read the same dvars (set by the scripts, or by the host's start packet).
restore_checkpoint_spot()
{
	if ( !isdefined( level.start_point ) || level.start_point == "default" || GetDvar( "mw2coop_cp_start" ) != level.start_point )
		return;
	parts = StrTok( GetDvar( "mw2coop_cp_pos" ), "_" );
	if ( parts.size != 4 )
		return;
	spot = ( spot_value( parts[ 0 ] ), spot_value( parts[ 1 ] ), spot_value( parts[ 2 ] ) );
	spawns = GetEntArray( "info_player_start", "classname" );
	if ( !spawns.size || level.player IsLinked() )
		return;
	if ( DistanceSquared( level.player.origin, spawns[ 0 ].origin ) > 128 * 128 )
		return;	// the start point placed him itself
	if ( DistanceSquared( level.player.origin, spot ) < 512 * 512 )
		return;
	level.player SetOrigin( spot );
	level.player SetPlayerAngles( ( 0, spot_value( parts[ 3 ] ), 0 ) );
	state( "start point " + level.start_point + " left player 1 at the level's spawn: moved to the checkpoint's spot " + spot );
}

// A level's end goes on to the next mission only from its default start (No Russian, The Enemy of
// My Enemy, Second Sun, Whiskey Hotel: "DEVELOPER: END OF SCRIPTED LEVEL" otherwise); a co-op
// checkpoint is a start point, so in co-op the end always goes on (playtest 2026-10-23: No Russian
// restarted at "tarmac" never ended).
ending_goes_on()
{
	return is_default_start() || is_campaign_coop();
}

// --------------------------------------------------------------------------------------------
// Owner, 2026-10-23: whoever reaches a checkpoint first, the other player is brought to him.
// Only a free player is moved: one linked to something (a vehicle, a scene, the Cliffhanger
// climb) stays where the level put him, and a downed one is left to the revive system.

player_label( player )
{
	if ( player == level.player )
		return "player 1";
	return "player 2";
}

// True when any player has ai in his field of view (cosine of the half angle: level.cosine[ "60" ]). Level scripts that make allies hurry only out of sight ("slingshot": Wolverines' allies run at 2.5 times
// the speed when the player does not see them) ask level.player alone: with the host ahead and the client trailing, they sprinted in front of the client's eyes.
any_player_sees( ai, cosine )
{
	foreach ( player in level.players )
		if ( IsAlive( player ) && within_fov( player.origin, player GetPlayerAngles(), ai.origin, cosine ) )
			return true;
	return false;
}

// True when any player is touching the entity (a trigger or volume). Level scripts that keep an ally or a scene waiting "until the
// player is out of the blast zone" ask level.player alone.
any_player_touching( volume )
{
	foreach ( player in level.players )
		if ( IsAlive( player ) && player IsTouching( volume ) )
			return true;
	return false;
}

// A player stuck off the ground (not linked, not standing: fallen into the rock, left on a
// ledge) while the other stands: the standing one leads. Else unknown.
standing_leader()
{
	if ( !isdefined( level.player2 ) )
		return undefined;
	foreach ( player in level.players )
	{
		other = other_player( player );
		if ( is_standing( player ) && !other IsLinked() && !is_standing( other ) )
			return player;
	}
	return undefined;
}

// Owner, 2026-11-04: the partner is brought to a checkpoint only when he is two checkpoints behind the
// player who reached it (2026-11-06: five; a partner a few sections behind walks on, as the leader waits for him or he catches up). Each player's progress is the number of the last real checkpoint he reached
// himself or came near (progress_watch), so a partner who is just a section behind walks on.
checkpoint_lead()
{
	return 5;
}

// A real checkpoint (not a developers' start, not the one the level began at) was reached: its number.
note_checkpoint_rank()
{
	if ( !isdefined( level.mw2coop.cp_rank ) )
	{
		level.mw2coop.cp_rank = 0;
		level.mw2coop.cp_spots = [];
		thread progress_watch();
	}
	level.mw2coop.cp_rank++;
	return level.mw2coop.cp_rank;
}

// self: nothing. The player's progress: the checkpoints before the first one reached count as reached.
progress_of( player )
{
	if ( !isdefined( player.mw2coop_progress ) )
		player.mw2coop_progress = 0;
	return player.mw2coop_progress;
}

checkpoint_spot_origin( leader )
{
	if ( isdefined( leader ) )
		return leader.origin;
	return level.player.origin;
}

// A player near the place where a checkpoint was reached has reached it too.
progress_watch()
{
	level endon( "mw2coop_level_end" );
	for ( ;; )
	{
		wait 0.5;
		foreach ( player in level.players )
		{
			if ( !IsAlive( player ) )
				continue;
			have = progress_of( player );
			for ( rank = have + 1; rank <= level.mw2coop.cp_rank; rank++ )
				if ( isdefined( level.mw2coop.cp_spots[ rank ] ) && DistanceSquared( player.origin, level.mw2coop.cp_spots[ rank ] ) < 700 * 700 )
					player.mw2coop_progress = rank;
		}
	}
}

bring_partner_to( leader, rank )
{
	if ( !isdefined( leader ) || !isdefined( level.player2 ) )
		return;
	other = other_player( leader );
	if ( rank - progress_of( other ) < checkpoint_lead() )
	{
		state( "checkpoint " + rank + ": " + player_label( other ) + " is at checkpoint " + progress_of( other ) + ", less than " + checkpoint_lead() + " behind: not brought" );
		return;
	}
	if ( !IsAlive( other ) || other IsLinked() || is_downed( other ) || is_downed( leader ) )
		return;
	if ( DistanceSquared( other.origin, leader.origin ) < 400 * 400 && is_standing( other ) )
		return;
	other thread brought_to( leader );
}

// self: the player to move
brought_to( leader )
{
	level endon( "mw2coop_level_end" );
	self endon( "death" );
	if ( self teleport_to_partner_when_safe( leader, 10 ) )
	{
		self.mw2coop_progress = leader.mw2coop_progress;
		state( player_label( self ) + " brought to " + player_label( leader ) + " at the checkpoint" );
	}
	else
		state( "no safe spot next to " + player_label( leader ) + " for " + player_label( self ) );
	self notify( "mw2coop_brought" );
}

// The developers' test starts: they skip ahead or leave the level half set up (the knife fight's
// later steps only start from the step before them: wakefast froze the game with an endless
// anim_loop_packet error). Not used as checkpoints; the level tour skips them too.
start_is_dev( name )
{
	foreach ( word in strtok( "debug test tweak nofail no_game alt_ending ending_ diskdrive fakerpg so_ free black body_throw noslowmo", " " ) )
		if ( IsSubStr( name, word ) )
			return true;
	foreach ( word in strtok( "wakefast turnbuckle gloat gun_drop crawl gun_kick wounded pullout kill price_wakeup walkoff jump e3", " " ) )
		if ( name == word )
			return true;
	return false;
}

checkpoint_hud()
{
	hud = NewClientHudElem( self );
	hud.x = 0;
	hud.y = -60;
	hud.alignX = "center";
	hud.alignY = "middle";
	hud.horzAlign = "center";
	hud.vertAlign = "middle";
	hud.font = "objective";
	hud.fontscale = 1.5;
	hud.foreground = true;
	hud.hidewheninmenu = true;
	hud.alpha = 1;
	hud SetText( &"SCRIPT_CHECKPOINT" );
	wait 2;
	hud FadeOverTime( 1 );
	hud.alpha = 0;
	wait 1;
	hud Destroy();
}

// --------------------------------------------------------------------------------------------
// Failure: both players down (maps\_coop revive system) or a scripted mission failure.

watch_failure()
{
	level endon( "mw2coop_level_end" );
	foreach ( player in level.players )
		player thread fail_on_death();
	while ( !isdefined( level.flag[ "coop_alldowned" ] ) )
		wait 0.05;
	flag_wait( "coop_alldowned" );
	// own thread: mission_failed() sends mw2coop_level_end, which would end this thread
	level thread mission_failed();
}

// A player's difficulty for maps\_gameskill::updateGameSkill(): in a co-op campaign the one the
// host picked (mw2coop_skill, sent to the client with coopLaunch, src/progress.c), for both
// players on both machines; the game took each player's from his own profile, so the two could
// play different ones and the level the higher. Elsewhere, or without a pick, the profile's.
player_gameskill()
{
	// the split screen lobby's own pick for each player (the console game's two difficulty popups; src/menus.c sets mw2coop_skill_p1 / p2, empty otherwise)
	pick = "mw2coop_skill_p1";
	if ( isdefined( level.player2 ) && self == level.player2 )
		pick = "mw2coop_skill_p2";
	own = GetDvar( pick );
	if ( is_campaign_coop() && ( own == "0" || own == "1" || own == "2" || own == "3" ) )
		return int( own );
	skill = GetDvar( "mw2coop_skill" );
	if ( is_campaign_coop() && ( skill == "0" || skill == "1" || skill == "2" || skill == "3" ) )
		return int( skill );
	return self maps\_utility::get_player_gameskill();
}

// Friendly fire between the players: the host's lobby setting (mw2coop_ff, "0" off; sent to the
// client with coopLaunch). On, the game's own co-op lets them hurt each other (QA pvp: a burst of
// M4 fire took the partner from 100 to 2). Off, g_friendlyfireDamageScale 0 leaves 1 damage a
// hit (QA pvp0: 100 to 92), which is given back.
friendly_fire_setting()
{
	// a saved dvar outlives the level: it is set at every level start either way. Off was set and never undone, so friendly fire stayed off in an
	// "on" session played after an "off" one, and a restart during Team Player's ride (maps\_mw2coop_roadkill ride_peace sets 0 until the ride
	// ends) left it at 0 (night audit 2026-11-17, GSC-4).
	if ( GetDvar( "mw2coop_ff" ) != "0" )
	{
		SetSavedDvar( "g_friendlyfireDamageScale", 1 );
		return;
	}
	SetSavedDvar( "g_friendlyfireDamageScale", 0 );
	foreach ( player in level.players )
		player thread give_back_partner_damage();
	state( "friendly fire between players off" );
}

// self: a player. Damage the other player did to him comes back.
give_back_partner_damage()
{
	self endon( "death" );
	level endon( "mw2coop_level_end" );
	for ( ;; )
	{
		self waittill( "damage", amount, attacker );
		if ( !isdefined( attacker ) || !IsPlayer( attacker ) || attacker == self )
			continue;
		self.health = int( min( self.maxhealth, self.health + amount ) );
	}
}

// A field of view only one player should get (the Predator's UAV view sets cg_fov for both
// screens): mw2coop_fov_pilot names that player ("1" or "2", "" when it ends), and the DLL keeps
// the other player's own field of view on his machine (src/fov.c).
fov_pilot( player )
{
	if ( !is_campaign_coop() )
		return;
	if ( !isdefined( player ) )
		SetDvar( "mw2coop_fov_pilot", "" );
	else if ( isdefined( level.player2 ) && player == level.player2 )
		SetDvar( "mw2coop_fov_pilot", "2" );
	else
		SetDvar( "mw2coop_fov_pilot", "1" );
}

// maps\_coop calls resetSkill() on every down and revive (so the player still up gets Spec Ops'
// small buff). It also puts both players' threatbias and attackeraccuracy back to the
// difficulty's values, which in the campaign undid a scene's own (Loose Ends' accuracy ramp,
// Team Player's and The Only Easy Day's threatbias; QA sceneskill, 2026-10-22). They are kept.
reset_skill_keep_scene()
{
	if ( !is_campaign_coop() )
	{
		maps\_gameskill::resetSkill();
		return;
	}
	threatbias = [];
	accuracy = [];
	foreach ( n, player in level.players )
	{
		threatbias[ n ] = player.threatbias;
		accuracy[ n ] = player.attackeraccuracy;
	}
	maps\_gameskill::resetSkill();
	foreach ( n, player in level.players )
	{
		player.threatbias = threatbias[ n ];
		if ( !player ent_flag( "player_zero_attacker_accuracy" ) )
			player.attackeraccuracy = accuracy[ n ];
	}
}

// A player killed outright (a scripted Kill(), as when he steps off Cliffhanger's cave ledge,
// owner 2026-10-22) is not downed and cannot be revived: the mission fails, as his death does in
// single player. Without this the other player played on beside a dead partner.
fail_on_death()
{
	level endon( "mw2coop_level_end" );
	self waittill( "death" );
	if ( isdefined( level.player2 ) && self == level.player2 )
		state( "player 2 died" );
	else
		state( "player 1 died" );
	level thread mission_failed();
}

// Replaces MissionFailed() in a co-op campaign session.
mission_failed()
{
	if ( level.mw2coop.failing || level.mw2coop.completing )
		return;
	level.mw2coop.failing = true;
	level notify( "mw2coop_level_end" );
	flag_set( "special_op_terminated" );
	// both machines run this in lockstep: their DLLs read mw2coop_retry when the host's fast_restart (and the coopRestart it sends the client) runs, 3 s from now
	if ( saves_on() )
	{
		SetDvar( "mw2coop_retry", "load" );
		state( "mission failed (deadquote '" + GetDvar( "ui_deadquote" ) + "'), back to the last checkpoint" );
	}
	else
	{
		SetDvar( "mw2coop_retry", "restart" );
		state( "mission failed (deadquote '" + GetDvar( "ui_deadquote" ) + "'), restart at '" + GetDvar( "start" ) + "'" );
	}
	foreach ( player in level.players )
	{
		player EnableInvulnerability();
		player FreezeControls( true );
		overlay = create_client_overlay( "black", 0, player );
		overlay FadeOverTime( 2 );
		overlay.alpha = 1;
	}
	wait 3;
	request( "restart" );	// the host runs fast_restart; the engine sends coopRestart to the client
}

// --------------------------------------------------------------------------------------------
// Mission end: replaces maps\_endmission::_nextmission() in a co-op campaign session.
// ChangeLevel() does nothing in a co-op session: its exit handler (rva 0x170e60) returns early
// when gameMode is "so". The host's DLL starts the next mission the way the lobby does
// (coop_mapName + coopLaunch, which also sends the client there). The game's records
// (highestMission, difficulty) are local-player profile data and are not written.
// Not handled yet: _nextmission()'s cliffhanger -> airport special case.

next_level_name()
{
	settings = level.missionSettings;
	index = settings maps\_endmission::getLevelIndex( level.script );
	// This build's _nextmission() completes airport right after cliffhanger ("always skip
	// airport for RUSSIAN PC BUILD", with no build check). The owner wants No Russian played:
	// co-op always goes on with airport.
	if ( !isdefined( index ) || index + 1 >= settings.levels.size )
		return "";
	return settings maps\_endmission::getLevelName( index + 1 );
}

mission_complete()
{
	if ( level.mw2coop.completing || level.mw2coop.failing )
		return;
	level.mw2coop.completing = true;
	level notify( "mw2coop_level_end" );
	flag_set( "special_op_terminated" );
	level notify( "nextmission" );
	level.nextmission = true;
	foreach ( player in level.players )
		player EnableInvulnerability();
	next = next_level_name();
	SetDvar( "start", "" );
	write_records();
	// The last mission: single player's _nextmission() waits for the end of the credits after the knife fight (af_chase_nextmission, set by
	// char_museum when the credits are over) and then goes back to the main menu. Here the request came 4 s into the credits and asked for a next
	// mission there is none of: both players stayed in the Museum with no way on (audit 2026-11-18, GSC F3). The request now waits for the same
	// flag; with no next mission both PCs leave the session for the main menu (src/control.c).
	if ( level.script == "ending" && isdefined( level.level_mode ) && level.level_mode == "credits_1" && isdefined( level.flag[ "af_chase_nextmission" ] ) )
	{
		state( "mission complete ending: waiting for the end of the credits" );
		flag_wait( "af_chase_nextmission" );
	}
	state( "mission complete " + level.script + ", next '" + next + "'" );
	request( "nextmap " + next );	// the host saves it as progress and launches it
}

// The campaign records _nextmission() writes in single player, on each machine into its own
// player's profile: the mission's best difficulty and the furthest mission (setLevelCompleted:
// a mission after one never completed is not counted, as in single player), the campaign
// percentage, the profile save, and the mission's achievements. Profile data is local and
// changes no game state, so the two lockstep machines may differ here. Endgame (af_chase) is
// recorded with the ending, as _nextmission( true ) does.
//
// The profile methods only answer for this machine's own player (on the client, level.player is
// the host's player and reads an empty profile), and _endmission works on level.player: it is
// that player for the few calls below, which never wait, so no other script sees the swap.
write_records()
{
	settings = level.missionSettings;
	script = level.script;
	if ( script == "af_chase" )
		return;
	if ( script == "ending" )
		script = "af_chase";
	index = settings maps\_endmission::getLevelIndex( script );
	if ( !isdefined( index ) )
		return;
	own = undefined;
	foreach ( player in level.players )
		if ( player GetLocalPlayerProfileData( "missionHighestDifficulty" ) != "" )
			own = player;
	if ( !isdefined( own ) )
		return;
	before = own GetLocalPlayerProfileData( "highestMission" ) + " " + own GetLocalPlayerProfileData( "missionHighestDifficulty" );
	player1 = level.player;
	level.player = own;
	settings maps\_endmission::setLevelCompleted( index );
	maps\_endmission::updateSpPercent();
	UpdateGamerProfile();
	if ( settings maps\_endmission::hasAchievement( index ) )
		maps\_utility::giveachievement_wrapper( settings maps\_endmission::getAchievement( index ) );
	level.player = player1;
	after = own GetLocalPlayerProfileData( "highestMission" ) + " " + own GetLocalPlayerProfileData( "missionHighestDifficulty" );
	who = "player 1";
	if ( own != level.player )
		who = "player 2";
	state( "records " + script + " (index " + index + "), " + who + ": highest mission and difficulties " + before + " -> " + after );
	wait 0.3;	// the DLL reads mw2coop_state every 250 ms
}

// --------------------------------------------------------------------------------------------
// Vehicles: level scripts seat player 1 only. These seat the partner on a free seat of the
// same vehicle (an AI seat the vehicle has not filled), linked the way the level links
// player 1, and take him out next to player 1 again.

// self: vehicle. A sittag no rider uses, rear seats first; skip_tag: player 1's seat.
free_seat_tag( skip_tag )
{
	if ( !isdefined( self.vehicletype ) || !isdefined( level.vehicle_aianims ) || !isdefined( level.vehicle_aianims[ self.vehicletype ] ) )
		return undefined;
	seats = level.vehicle_aianims[ self.vehicletype ];
	for ( pos = seats.size - 1; pos > 0; pos-- )	// never the driver (0)
	{
		if ( !isdefined( seats[ pos ].sittag ) || ( isdefined( skip_tag ) && seats[ pos ].sittag == skip_tag ) )
			continue;
		if ( isdefined( self.usedPositions ) && isdefined( self.usedPositions[ pos ] ) && self.usedPositions[ pos ] )
			continue;
		if ( IsSubStr( seats[ pos ].sittag, "turret" ) )
			continue;
		self.usedPositions[ pos ] = true;	// keep AI riders off it
		self.mw2coop_seat_pos = pos;
		return seats[ pos ].sittag;
	}
	return undefined;
}

// The player on a vehicle turret shared by two players (a level sets level.mw2coop_gunner).
vehicle_gunner()
{
	if ( isdefined( level.mw2coop_gunner ) && IsAlive( level.mw2coop_gunner ) )
		return level.mw2coop_gunner;
	return level.player;
}

other_player( player )
{
	if ( player == level.player )
		return level.player2;
	return level.player;
}

// self: vehicle. Returns the seat entity (pass it to partner_leaves_seat() or release_seat()).
//   skip_tag: player 1's seat (default tag_passenger; "" skips none); weapons: usable on the seat.
seat_partner_in_vehicle( player, right, left, up, down, skip_tag, weapons )
{
	if ( !isdefined( player ) || !IsAlive( player ) )
		return undefined;
	if ( !isdefined( right ) )
		right = 90;
	if ( !isdefined( left ) )
		left = 90;
	if ( !isdefined( up ) )
		up = 40;
	if ( !isdefined( down ) )
		down = 40;
	if ( !isdefined( skip_tag ) )
		skip_tag = "tag_passenger";

	tag = self free_seat_tag( skip_tag );
	// a level can name a squadmate who gives up his seat (level.mw2coop_spare_rider)
	evicted = "";
	if ( !isdefined( tag ) && isdefined( level.mw2coop_spare_rider ) )
	{
		evicted = " spare " + level.mw2coop_spare_rider + ": " + self evict_rider( level.mw2coop_spare_rider );
		tag = self free_seat_tag( skip_tag );
	}
	seat = spawn_tag_origin();
	seat.mw2coop_vehicle = self;
	seat.mw2coop_arcs = [];
	seat.mw2coop_arcs[ 0 ] = right;
	seat.mw2coop_arcs[ 1 ] = left;
	seat.mw2coop_arcs[ 2 ] = up;
	seat.mw2coop_arcs[ 3 ] = down;
	seat.mw2coop_weapons = isdefined( weapons ) && weapons;
	if ( isdefined( tag ) )
	{
		seat.mw2coop_pos = self.mw2coop_seat_pos;
		// same offset as maps\_utility::player_rides_shotgun_in_humvee() uses on tag_passenger
		seat LinkTo( self, tag, ( -5, 10, -34 ), ( 0, 0, 0 ) );
		state( "partner seated on " + tag + evicted );
	}
	else
	{
		seat LinkTo( self, "tag_body", ( -50, 0, 10 ), ( 0, 0, 0 ) );
		state( "partner: no free seat (" + self seats_text() + ")" + evicted + ", riding in the back" );
	}
	seat link_to_seat( player );
	return seat;
}

// self: seat. Puts player on it (also used when two players swap places).
link_to_seat( player )
{
	self.mw2coop_player = player;
	player AllowCrouch( false );
	player AllowProne( false );
	if ( self.mw2coop_weapons )
		player EnableWeapons();
	else
		player DisableWeapons();
	player PlayerLinkTo( self, "tag_origin", 0.8, self.mw2coop_arcs[ 0 ], self.mw2coop_arcs[ 1 ], self.mw2coop_arcs[ 2 ], self.mw2coop_arcs[ 3 ] );
}

// self: seat. The player gets off where he is; the seat is freed.
release_seat()
{
	player = self.mw2coop_player;
	if ( isdefined( self.mw2coop_vehicle ) && isdefined( self.mw2coop_pos ) && isdefined( self.mw2coop_vehicle.usedPositions ) )
		self.mw2coop_vehicle.usedPositions[ self.mw2coop_pos ] = false;
	if ( isdefined( player ) && IsAlive( player ) )
	{
		org = player.origin;
		player Unlink();
		player SetOrigin( org );
		player EnableWeapons();
		player AllowCrouch( true );
		player AllowProne( true );
	}
	self Delete();
}

// self: seat from seat_partner_in_vehicle().
partner_leaves_seat( delay )
{
	if ( isdefined( delay ) )
		wait delay;
	player = self.mw2coop_player;
	if ( isdefined( self.mw2coop_vehicle ) && isdefined( self.mw2coop_pos ) && isdefined( self.mw2coop_vehicle.usedPositions ) )
		self.mw2coop_vehicle.usedPositions[ self.mw2coop_pos ] = false;
	if ( isdefined( player ) && IsAlive( player ) )
	{
		player Unlink();
		player EnableWeapons();
		player AllowCrouch( true );
		player AllowProne( true );
		player thread teleport_to_partner_when_safe( other_player( player ), 5 );
	}
	self Delete();
}

// self: vehicle. "type: pos=tag used ..." for the log.
seats_text()
{
	if ( !isdefined( self.vehicletype ) )
		return "no vehicletype";
	if ( !isdefined( level.vehicle_aianims ) || !isdefined( level.vehicle_aianims[ self.vehicletype ] ) )
		return self.vehicletype + ": no seat anims";
	text = self.vehicletype + ":";
	seats = level.vehicle_aianims[ self.vehicletype ];
	for ( pos = 0; pos < seats.size; pos++ )
	{
		tag = "?";
		if ( isdefined( seats[ pos ].sittag ) )
			tag = seats[ pos ].sittag;
		used = isdefined( self.usedPositions ) && isdefined( self.usedPositions[ pos ] ) && self.usedPositions[ pos ];
		text += " " + pos + "=" + tag;
		if ( used )
			text += "*";
		if ( isdefined( self.riders ) )
			foreach ( guy in self.riders )
				if ( isdefined( guy ) && isdefined( guy.vehicle_position ) && guy.vehicle_position == pos )
					text += "(" + rider_name( guy ) + ")";
	}
	return text;
}

rider_name( guy )
{
	if ( isdefined( guy.animname ) )
		return guy.animname;
	if ( isdefined( guy.script_noteworthy ) )
		return guy.script_noteworthy;
	if ( isdefined( guy.targetname ) )
		return guy.targetname;
	return "ai";
}

// self: vehicle. Removes the rider with this name (a squadmate the level does not need)
// so a player can take his seat. The vehicle frees the seat when the rider is gone
// (maps\_vehicle_aianim::guy_deathhandle); done here too in case that runs later.
evict_rider( animname )
{
	if ( !isdefined( self.riders ) )
		return "no riders";
	names = "";
	foreach ( guy in self.riders )
	{
		if ( isdefined( guy ) )
			names += rider_name( guy ) + " ";
		// animname, or the spawner's script_noteworthy while spawn functions have not run yet
		if ( !isdefined( guy ) || rider_name( guy ) != animname )
			continue;
		pos = guy.vehicle_position;
		self.riders = array_remove( self.riders, guy );
		if ( isdefined( guy.magic_bullet_shield ) )
			guy stop_magic_bullet_shield();
		guy Delete();
		if ( isdefined( pos ) && isdefined( self.usedPositions ) )
			self.usedPositions[ pos ] = false;
		return "removed";
	}
	return "not among " + names;
}

// Campaign players have no body model (Spec Ops gives them coop_body_* models, which only the
// so_* zones hold), so there is no "tag_origin" on them for effects or links. self: player.
// Returns a tag_origin model that follows him, for code that plays effects on a player's tag.
player_tag_ent()
{
	if ( !isdefined( self.mw2coop_tag_ent ) )
	{
		ent = spawn_tag_origin();
		ent.origin = self.origin;
		ent.angles = self.angles;
		ent LinkTo( self );
		self.mw2coop_tag_ent = ent;
	}
	return self.mw2coop_tag_ent;
}

// The player a sound filter (EQ, reverb) is for: the caller when it is a player (maps\_gameskill's near-death filter runs on the hurt
// player), else the host (the level's own ambience). See patches/maps/_ambient.gsc.patch.
sound_player( caller )
{
	if ( isdefined( caller ) && isplayer( caller ) )
		return caller;
	return level.player;
}

// maps\_slowmo_breach patch: right after a breach lets the players go (door: the breach's door post, room: the centre of the breached room's volume). Each player is
// watched for 15 s or until he has walked away from where he was let go; one who is stuck is moved into the room. Stuck: he cannot take a step from where he stands
// (spot_stuck: the world's geometry), or, near the door, he pushes his stick or keys for half a second of game time (about two seconds in the breach's slow motion)
// and does not move at all. The second case is Just Like Old Times'
// control room (owner: "one of the players stays stuck at the door after the breach"; QA breachstuck): the co-op animation of the player who set the charge
// (breach_coop_player_1) ends in a solid area around the blown door that spot_stuck does not see (it traces the world only), and the spot beside the other player
// that teleport_to_partner picked was inside it too; 40 units further into the room he walks (his speed, the breach state and the door's path clip were not it).
// So he is moved 40 units into the room (towards the room's centre), again if he is still stuck, and only then next to the other player.
breach_unstick( door, room )
{
	level endon( "mw2coop_level_end" );
	if ( GetDvar( "mw2coop_qa_nounstick" ) == "1" )	// QA breachstuck: the breach as the game leaves it, to see the stuck player himself
	{
		breach_objectives_back();
		return;
	}
	foreach ( player in level.players )
		player thread breach_unstick_watch( door, room );
	breach_objectives_back();
}

breach_unstick_watch( door, room )
{
	self endon( "death" );
	level endon( "mw2coop_level_end" );
	start = self.origin;
	last = self.origin;
	pushed = 0;
	moves = 0;
	for ( t = 0; t < 15; t += 0.1 )
	{
		wait 0.1;
		if ( !IsAlive( self ) || self IsLinked() || is_downed( self ) )
		{
			start = self.origin;
			last = self.origin;
			pushed = 0;
			continue;
		}
		if ( Distance( start, self.origin ) > 48 + 40 * moves )
			return;	// he walked away
		reason = undefined;
		if ( spot_stuck( self.origin ) )
			reason = "inside the level's geometry";
		near = !isdefined( door ) || Distance2D( self.origin, door ) < 160;
		if ( near && Length( self GetNormalizedMovement() ) > 0.5 && Distance( last, self.origin ) < 0.3 )
			pushed += 0.1;
		else
			pushed = 0;
		last = self.origin;
		if ( !isdefined( reason ) && pushed >= 0.5 )
			reason = "pushes to move and does not move";
		if ( !isdefined( reason ) )
			continue;
		who = "player 2";
		if ( self == level.player )
			who = "player 1";
		if ( isdefined( door ) && isdefined( room ) && moves < 3 )
		{
			into = VectorNormalize( ( room[ 0 ] - door[ 0 ], room[ 1 ] - door[ 1 ], 0 ) );
			to = self.origin + into * 40;
			ground = PhysicsTrace( to + ( 0, 0, 40 ), to - ( 0, 0, 80 ) );
			if ( !spot_stuck( ground ) && abs( ground[ 2 ] - self.origin[ 2 ] ) < 40 )
			{
				moves++;
				state( "breach: " + who + " stuck after the breach at " + self.origin + " (" + reason + "), moved 40 units into the room (" + moves + ")" );
				self SetOrigin( ground );
				last = self.origin;
				pushed = 0;
				continue;
			}
		}
		other = other_player( self );
		if ( !isdefined( other ) || !IsAlive( other ) || spot_stuck( other.origin ) )
			continue;
		state( "breach: " + who + " stuck after the breach at " + self.origin + " (" + reason + "), put next to the other player" );
		self thread teleport_to_partner_when_safe( other, 5 );
		return;
	}
}

// After a breach: the objectives' markers come back once its slow motion is over, whether or not its own thread got that far (see main()).
breach_objectives_back()
{
	level endon( "mw2coop_level_end" );
	for ( t = 0; t < 20 && isdefined( level.breaching ) && level.breaching; t += 0.5 )
		wait 0.5;
	wait 1;
	if ( !isdefined( level.breaching ) || !level.breaching )
		SetSavedDvar( "objectiveHide", false );
}

// Hints that belong to one player: the holder of an item (the Exodus laser, a Predator laptop: each holder gets his own copy), the driver of a vehicle, a climber,
// a rappeller, the lead of a scene (boat, knife fight), the player a mine threw, the shooter of an armoured vehicle. maps\_utility's display_hint is not mirrored
// to player 2 for these (_utility.gsc.patch hunk 7): with player 1 holding the laser, player 2 was told to press the d-pad for a designator he does not have.
// From the night audit of 2026-11-17 (every display_hint call site of the campaign read).
hint_is_own( hint )
{
	if ( !isdefined( hint ) )
		return true;
	if ( !isdefined( level.mw2coop_hint_own ) )
	{
		level.mw2coop_hint_own = [];
		names = StrTok( "use_laser use_laser_golf invulerable_bullets invulerable_frags zodiac_drive zodiac_attack zodiac_reverse hint_crawl_left hint_crawl_right hint_melee left_icepick right_icepick how_to_climb rappel_melee begin_descent barrett how_to_pop_flare hint_predator_drone_1 hint_predator_drone_2 hint_predator_drone_3 hint_predator_drone_4 hint_dont_leave_price", " " );
		foreach ( name in names )
			level.mw2coop_hint_own[ name ] = true;
	}
	if ( isdefined( level.mw2coop_hint_own[ hint ] ) )
		return true;
	return IsSubStr( hint, "_player1" ) || IsSubStr( hint, "_player2" ) || IsSubStr( hint, "mineavoid_hint" );
}

// --------------------------------------------------------------------------------------------
// Night audit of 2026-11-17 (every place where the game's code is tied to one player): helpers for the patches that make those places per player.

// maps\_utility helpers that act on "the player" (set_player_attacker_accuracy(), ...) fall back to player 1 when they are called on the level or on an
// entity that is no player (get_player_from_self()). Such a call is about the scene, which both players are in: true when it should be done to both.
// A call made on one player stays his.
for_both_players( caller )
{
	if ( !is_campaign_coop() || !isdefined( level.player2 ) )
		return false;
	return !isdefined( caller ) || !is_in_array( level.players, caller );
}

// A level that waits for "the player" to throw a grenade (level.player waittill( "grenade_fire" ), e.g. the smoke that marks Wolverines' BMP) waits for
// either player's. Returns a struct: .grenade, .weapon and .player of the first throw (of that weapon, when one is named).
any_player_grenade_fire( weapon_name )
{
	relay = SpawnStruct();
	foreach ( player in level.players )
		player thread grenade_fire_relay( relay, weapon_name );
	relay waittill( "mw2coop_grenade", grenade, weapon, player );
	relay notify( "mw2coop_done" );
	result = SpawnStruct();
	result.grenade = grenade;
	result.weapon = weapon;
	result.player = player;
	return result;
}

grenade_fire_relay( relay, weapon_name )
{
	relay endon( "mw2coop_done" );
	self endon( "death" );
	for ( ;; )
	{
		self waittill( "grenade_fire", grenade, weapon );
		if ( !isdefined( weapon_name ) || weapon == weapon_name )
		{
			relay notify( "mw2coop_grenade", grenade, weapon, self );
			return;
		}
	}
}

// The highest ammo stock of a weapon any player has (a level that asks whether "the player" still has smoke or C4).
players_ammo_stock( weapon )
{
	best = 0;
	foreach ( player in level.players )
	{
		n = player GetWeaponAmmoStock( weapon );
		if ( n > best )
			best = n;
	}
	return best;
}

// A free place for a player beside a spot where a scene puts the other player (a view node): 48 units to its right or left or behind it (the
// choice of teleport_to_partner), with floor at about its height and room around it; the spot itself when nothing near is free.
free_spot_beside( origin, angles, ignore_ent )
{
	yaw = ( 0, angles[ 1 ], 0 );
	forward = AnglesToForward( yaw );
	right = AnglesToRight( yaw );
	offsets = [];
	offsets[ offsets.size ] = right * 48;
	offsets[ offsets.size ] = right * -48;
	offsets[ offsets.size ] = forward * -48;
	offsets[ offsets.size ] = right * 64 + forward * -48;
	offsets[ offsets.size ] = right * -64 + forward * -48;
	offsets[ offsets.size ] = forward * -96;
	chest = origin + ( 0, 0, 48 );
	best = undefined;
	best_open = -1;
	foreach ( offset in offsets )
	{
		spot = origin + offset;
		if ( !BulletTracePassed( chest, spot + ( 0, 0, 48 ), false, ignore_ent ) )
			continue;
		ground = PlayerPhysicsTrace( spot + ( 0, 0, 48 ), spot - ( 0, 0, 96 ) );
		if ( abs( ground[ 2 ] - origin[ 2 ] ) > 40 || spot_stuck( ground ) )
			continue;
		open = open_sides( ground );
		if ( open >= 5 )
			return ground;
		if ( open > best_open )
		{
			best = ground;
			best_open = open;
		}
	}
	if ( isdefined( best ) )
		return best;
	return origin;
}

// A level's slowed walk for "the player" (maps\_utility blend_movespeedscale) set for both players: add_func() of a level's waits calls it on the
// level (Second Sun's lobby and parking deck).
players_blend_movespeedscale( scale, time )
{
	foreach ( player in level.players )
		player blend_movespeedscale( scale, time );
}

// A position as whole units, for the log ("x y z").
vec_text( v )
{
	if ( !isdefined( v ) )
		return "?";
	return int( v[ 0 ] ) + " " + int( v[ 1 ] ) + " " + int( v[ 2 ] );
}
