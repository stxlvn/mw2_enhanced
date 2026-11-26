// mw2coop: the Cliffhanger ice climb and big jump for two players (maps\_climb, a common script).
// Called from mw2coop/patches/maps/_climb.gsc.patch, _climb_anim.gsc.patch and
// cliffhanger.gsc.patch. Outside a co-op campaign session every entry point does what the
// original code did.
//
// The level starts maps\_climb::climb_wall for every player at its own "player_climb" start
// (the map has two lanes), but the climb code works on level.player and on level flags. The
// patches turn it per player:
//  - level.player -> the climbing player in every function of the climb (patch `subst`s);
//  - the climb's flags (trigger volumes and the climb's own state) become per-player entity
//    flags here, fed by the same trigger volumes (pflag*, flag_monitor);
//  - the swing buttons are read from the player's usercmd (attack/ads), not from local keys,
//    so both lockstep machines see the same input;
//  - player 2 climbs his own lane; the shared spots at the top (pull-up, big jump, final
//    climb, getting up) are moved over by the distance between the two lanes (lane_*);
//  - big jump: Soap catches the players in turn (catch_queued).
#include common_scripts\utility;
#include maps\_utility;
#include maps\_anim;

climb_coop()
{
	return maps\_mw2coop::is_campaign_coop();
}

// --------------------------------------------------------------------------------------------
// Setup, from maps\_climb::climb_init() (replaces its "thread player_slides_off_cliff()").

climb_setup()
{
	level.mw2coop_qa_climb = maps\_mw2coop_climb_qa::climb;
	level.mw2coop_qa_climbview = maps\_mw2coop_climb_qa::climbview;
	level.mw2coop_qa_climblb = maps\_mw2coop_climb_qa::climblb;
	if ( !climb_coop() )
	{
		level.player thread maps\_climb::player_slides_off_cliff();
		return;
	}
	level.mw2coop_climb = SpawnStruct();
	c = level.mw2coop_climb;
	level.mw2coop_own_weapons = [];	// each climber's own (maps\_mw2coop::own_weapon_busy)
	level.mw2coop_own_weapons[ 0 ] = "ice_picker";

	// flags fed by trigger volumes in the map: a player "has" the flag while he touches one
	// (flag_set_touching triggers), or from his first touch until the climb clears it again.
	// fade_to_death: the map's volume under the cave ledge (owner, 2026-10-22: in single player a
	// step off the ledge fades to black and fails the mission; in co-op it was fed by nothing, so
	// a player who stepped off fell forever).
	c.trigger_flags = [];
	foreach ( name in strtok( "player_in_position_to_climb climb_start climb_pullup climb_drop_down player_climbs_past_safe_point climb_jump_prep climb_big_jump climb_icepick_slide fade_to_death", " " ) )
	{
		triggers = flag_triggers( name );
		if ( !triggers.size )
		{
			maps\_mw2coop::state( "climb: no trigger for flag " + name + ", kept shared" );
			continue;
		}
		c.trigger_flags[ name ] = triggers;
	}
	// the climb's own state
	c.player_flags = [];
	foreach ( name in strtok( "we_care_about_right_icepick finished_climbing final_climb player_preps_for_jump player_makes_the_jump player_hangs_on player_was_caught price_caught_player climbing_dof fade_to_death reached_top player_starts_climbing", " " ) )
		c.player_flags[ name ] = true;
	foreach ( name, _ in c.trigger_flags )
		c.player_flags[ name ] = true;

	foreach ( player in level.players )
	{
		foreach ( name, _ in c.player_flags )
			if ( !player ent_flag_exist( name ) )
				player ent_flag_init( name );
		player thread flag_monitor();
		player thread maps\_climb::player_slides_off_cliff();
	}
	// the climb keeps the players on their lanes: no teleport to player 1 until both are up
	level.mw2coop_no_follow = true;
	thread end_of_climb();
	maps\_mw2coop::state( "climb: per-player flags ready, " + c.trigger_flags.size + " trigger flags" );
}

flag_triggers( name )
{
	found = [];
	triggers = getEntArrayWithFlag( name );
	foreach ( targetname in strtok( "flag_set flag_set_touching", " " ) )
		triggers = array_combine( triggers, GetEntArray( targetname, "targetname" ) );
	foreach ( trigger in triggers )
	{
		if ( !isdefined( trigger.script_flag ) || trigger.script_flag != name )
			continue;
		if ( !array_contains( found, trigger ) )
			found[ found.size ] = trigger;
	}
	return found;
}

is_touching_trigger( trigger )
{
	if ( IsSubStr( trigger.classname, "touching" ) )
		return true;
	return isdefined( trigger.targetname ) && trigger.targetname == "flag_set_touching";
}

// self: player. Sets his copy of each trigger flag while he touches its volume, as the level's
// flag triggers do for the shared flag.
flag_monitor()
{
	self endon( "death" );
	level endon( "reached_top" );
	c = level.mw2coop_climb;
	for ( ;; )
	{
		foreach ( name, triggers in c.trigger_flags )
		{
			touching = false;
			clears = false;
			cleared = false;
			foreach ( trigger in triggers )
			{
				if ( !isdefined( trigger ) )
					continue;
				if ( IsSubStr( trigger.classname, "clear" ) )
				{
					// flag_clear triggers clear the flag while touched
					if ( self IsTouching( trigger ) )
						cleared = true;
					continue;
				}
				if ( is_touching_trigger( trigger ) )
					clears = true;
				if ( self IsTouching( trigger ) )
					touching = true;
			}
			if ( cleared )
				touching = false;
			if ( touching && !self ent_flag( name ) )
				self ent_flag_set( name );
			else if ( !touching && ( clears || cleared ) && self ent_flag( name ) )
				self ent_flag_clear( name );
		}
		wait 0.05;
	}
}

end_of_climb()
{
	flag_wait( "reached_top" );
	level.mw2coop_no_follow = undefined;
	maps\_mw2coop::state( "climb: both players at the top" );
}

is_player_flag( name )
{
	return climb_coop() && isdefined( level.mw2coop_climb ) && isdefined( level.mw2coop_climb.player_flags[ name ] );
}

// --------------------------------------------------------------------------------------------
// Flags. self: the climbing player. A per-player flag set by one player also sets the shared
// flag (the rest of the level reads "a player did it"), except reached_top, which is set once
// every player has reached the top.

pflag( name )
{
	if ( !is_player_flag( name ) )
		return flag( name );
	return self ent_flag( name );
}

pflag_set( name )
{
	if ( !is_player_flag( name ) )
	{
		flag_set( name );
		return;
	}
	self ent_flag_set( name );
	if ( name == "reached_top" )
	{
		foreach ( player in level.players )
			if ( !player ent_flag( name ) )
			{
				maps\_mw2coop::state( "climb: " + player_name( self ) + " at the top, waiting for the other player" );
				// one not on the wall (fallen, stuck) is brought up and counts as at the top
				if ( !player IsLinked() )
					player thread brought_to_top( self );
				return;
			}
	}
	if ( isdefined( level.flag[ name ] ) )
		flag_set( name );
}

pflag_clear( name )
{
	if ( !is_player_flag( name ) )
	{
		flag_clear( name );
		return;
	}
	self ent_flag_clear( name );
	foreach ( player in level.players )
		if ( player ent_flag( name ) )
			return;
	if ( isdefined( level.flag[ name ] ) )
		flag_clear( name );
}

pflag_wait( name )
{
	if ( !is_player_flag( name ) )
	{
		flag_wait( name );
		return;
	}
	self ent_flag_wait( name );
}

pflag_waitopen( name )
{
	if ( !is_player_flag( name ) )
	{
		flag_waitopen( name );
		return;
	}
	self ent_flag_waitopen( name );
}

player_name( player )
{
	if ( isdefined( level.player2 ) && player == level.player2 )
		return "player 2";
	return "player 1";
}

// --------------------------------------------------------------------------------------------
// Input. self: player. The climb read the mouse buttons and the j/k keys with ButtonPressed(),
// which reads this PC's keyboard: in lockstep the other machine would see different input. In
// co-op the swings come from the player's usercmd, the buttons the developers' comments name
// (left: attack, right: aim). QA can hold buttons for a player through self.mw2coop_qa_hold.

swing_pressed( arm, key )
{
	if ( !climb_coop() )
		return level.player ButtonPressed( key );
	// tells the DLL (src/climbads.c) that the climb reads the aim button: the command of a player linked to the wall carries no right click otherwise
	if ( !isdefined( level.mw2coop_keepads_time ) || level.mw2coop_keepads_time != GetTime() )
	{
		level.mw2coop_keepads_time = GetTime();
		SetDvar( "mw2coop_keepads", "" + level.mw2coop_keepads_time );
	}
	if ( isdefined( self.mw2coop_qa_hold ) && isdefined( self.mw2coop_qa_hold[ arm ] ) && self.mw2coop_qa_hold[ arm ] )
		return true;
	if ( arm == "left" )
		return self AttackButtonPressed();
	return self AdsButtonPressed();
}

// maps\_climb::should_fall(): the time since the game was last paused, for its 10 s grace. GetTimeSinceLastPaused() is each PC's own: the co-op machines are
// paused at different moments (a failed mission's screen, a load) and one dropped the climber while the other did not. A co-op game is not paused for one
// player: no grace, the same on both machines.
time_since_paused()
{
	if ( !climb_coop() )
		return GetTimeSinceLastPaused();
	return 100000;
}

// ent: an arm of the climb. The extra keyboard keys (j, k) only outside co-op.
key_pressed( ent )
{
	if ( climb_coop() )
		return false;
	return ent.player ButtonPressed( ent.key );
}

// self: a player off the wall (fallen, stuck) while the other reached the top
brought_to_top( leader )
{
	level endon( "mw2coop_level_end" );
	self endon( "death" );
	wait 0.5;	// the leader gets to his feet first
	if ( self IsLinked() || self ent_flag( "reached_top" ) )
		return;
	if ( !self maps\_mw2coop::teleport_to_partner_when_safe( leader, 10 ) )
	{
		maps\_mw2coop::state( "climb: no safe spot at the top for " + player_name( self ) );
		return;
	}
	maps\_mw2coop::state( "climb: " + player_name( self ) + " brought to the top" );
	self pflag_set( "reached_top" );
}

// player_falls_to_death(), self: a player who let go of both picks on the first wall. Past the
// safe point the climb kills him; in co-op he drops back to the bottom from any height and climbs
// again (owner, 2026-10-23: "при срыве отбрасывать на старт"). The far wall after the jump stays
// deadly: below it is the drop.
deadly_height()
{
	if ( climb_coop() )
	{
		if ( self pflag( "player_climbs_past_safe_point" ) )
			maps\_mw2coop::state( "climb: " + player_name( self ) + " fell past the safe point, back to the bottom" );
		return false;
	}
	return flag( "player_climbs_past_safe_point" );
}

// A player who let go of both picks drops back to the bottom: the climb's player_recover, for
// both players. Player 2's own spots there were inside the ground at the foot of his lane
// (playtests 2026-10-23: the lane-shifted spot left him stuck in the slope, his lane's start put
// him 40 units under the floor); player 1's spot is ground (he landed and climbed again). Players
// do not push each other apart: one put inside the other is moved beside him.
recover_ent( player )
{
	if ( climb_coop() && isdefined( player ) )
		player thread recover_apart();
	return GetEnt( "player_recover", "targetname" );
}

// self: a player dropping back to the bottom. Once he stands there, if the other player stands on
// the same spot, he is moved beside him (teleport_to_partner: a free spot with floor at his
// height and a clear line from him, never over a drop).
recover_apart()
{
	level endon( "mw2coop_level_end" );
	self endon( "death" );
	self notify( "mw2coop_recover_apart" );
	self endon( "mw2coop_recover_apart" );
	for ( t = 0; t < 5 && ( self IsLinked() || !maps\_mw2coop::is_standing( self ) ); t += 0.05 )
		wait 0.05;
	other = maps\_mw2coop::other_player( self );
	if ( !isdefined( other ) || other IsLinked() || DistanceSquared( self.origin, other.origin ) > 32 * 32 )
		return;
	if ( self maps\_mw2coop::teleport_to_partner_when_safe( other, 2 ) )
		maps\_mw2coop::state( "climb: " + player_name( self ) + " recovered on the other player, moved beside him" );
}

// --------------------------------------------------------------------------------------------
// Lanes. Player 2 climbs the second lane; the single spots the climb uses at the top are moved
// for him by the distance between the lanes (across the wall, height kept).

// cave_main(): the climb start of the index-th player. The map has one "player_climb" start;
// the second player gets a lane beside it, on the side away from Soap's climb, where the wall
// has ice (or rock or snow: what the icepicks take) all the way up.
climb_start( starts, index )
{
	if ( isdefined( starts[ index ] ) )
		return starts[ index ];
	if ( isdefined( level.mw2coop_second_lane ) )
		return level.mw2coop_second_lane;
	base = starts[ 0 ];
	forward = AnglesToForward( ( 0, base.angles[ 1 ], 0 ) );
	right = AnglesToRight( ( 0, base.angles[ 1 ], 0 ) );
	away = 1;
	soap = GetEnt( "cliffhanger_cliff", "targetname" );
	if ( isdefined( soap ) && VectorDot( soap.origin - base.origin, right ) > 0 )
		away = -1;
	best = undefined;
	best_score = -1;
	dists = [];
	dists[ 0 ] = 72;
	dists[ 1 ] = 88;
	dists[ 2 ] = 56;
	dists[ 3 ] = 104;
	foreach ( dist in dists )
	{
		for ( side = 1; side >= -1; side -= 2 )
		{
			offset = right * ( dist * away * side );
			score = wall_score( base.origin + offset, forward );
			if ( score > best_score )
			{
				best_score = score;
				best = offset;
			}
		}
		if ( best_score >= 12 )
			break;
	}
	lane = SpawnStruct();
	lane.origin = base.origin + best;
	lane.angles = base.angles;
	level.mw2coop_second_lane = lane;
	maps\_mw2coop::state( "climb: second lane at " + best + " from the first, wall score " + best_score + "/12" );
	return lane;
}

// How many of 12 points up the wall in front of org the icepicks can take.
wall_score( org, forward )
{
	score = 0;
	for ( h = 0; h < 12; h++ )
	{
		at = org + ( 0, 0, h * 50 );
		trace = BulletTrace( at - forward * 40, at + forward * 120, false, undefined );
		if ( trace[ "fraction" ] < 1 && isdefined( trace[ "surfacetype" ] ) )
		{
			surface = trace[ "surfacetype" ];
			if ( surface == "ice" || surface == "rock" || surface == "snow" || surface == "plaster" )
				score++;
		}
	}
	return score;
}

// self: player, from climb_wall().
note_lane( start_org )
{
	self.mw2coop_climb_start = start_org;
}

lane_offset( player )
{
	if ( !climb_coop() || !isdefined( player ) || player == level.player )
		return ( 0, 0, 0 );
	if ( isdefined( player.mw2coop_lane_offset ) )
		return player.mw2coop_lane_offset;
	offset = undefined;
	if ( isdefined( player.mw2coop_climb_start ) && isdefined( level.player.mw2coop_climb_start ) )
	{
		d = player.mw2coop_climb_start - level.player.mw2coop_climb_start;
		offset = ( d[ 0 ], d[ 1 ], 0 );
	}
	if ( !isdefined( offset ) || Length( offset ) < 32 )
	{
		// start points past the wall: beside player 1, to the right of the jump
		node = GetEnt( "climb_jump_org", "targetname" );
		offset = AnglesToRight( ( 0, node.angles[ 1 ], 0 ) ) * 64;
	}
	player.mw2coop_lane_offset = offset;
	maps\_mw2coop::state( "climb: player 2 lane offset " + offset );
	return offset;
}

// Across the gap (big jump, Soap's catch, the second wall, getting up at the top): player 2's
// offset from player 1's spots.
far_offset( player )
{
	if ( !climb_coop() || !isdefined( player ) || player == level.player )
		return ( 0, 0, 0 );
	// Player 2 uses player 1's own spots across the gap (the jump, the hang, Soap's catch, the far
	// wall): the second lane there scored 6 of 12 and its hang spot under the edge was inside the
	// rock (playtest 2026-10-23: player 2 thrown behind the wall's texture). Soap catches the
	// players in turn, so the second one hangs where the first did; two hanging at once overlap.
	if ( !isdefined( level.mw2coop_far_offset ) )
	{
		level.mw2coop_far_offset = ( 0, 0, 0 );
		maps\_mw2coop::state( "climb: player 2 uses player 1's spots across the gap" );
	}
	return level.mw2coop_far_offset;
}

// An entity standing for the named entity on the player's lane (player 1: the entity itself).
// side: "near" (the first wall) or "far" (across the gap). climb_catch is kept in co-op (see
// keep_or_delete), so the original is always there to copy.
lane_ent( name, player, side )
{
	ent = GetEnt( name, "targetname" );
	if ( !climb_coop() || !isdefined( player ) || player == level.player )
		return ent;
	if ( !isdefined( level.mw2coop_lane_ents ) )
		level.mw2coop_lane_ents = [];
	key = name + " " + side;
	if ( !isdefined( level.mw2coop_lane_ents[ key ] ) )
	{
		if ( side == "far" )
			offset = far_offset( player );
		else
			offset = lane_offset( player );
		copy = Spawn( "script_origin", ent.origin + offset );
		copy.angles = ent.angles;
		level.mw2coop_lane_ents[ key ] = copy;
	}
	return level.mw2coop_lane_ents[ key ];
}

lane_struct( name, player, side )
{
	s = getstruct( name, "targetname" );
	if ( !climb_coop() || !isdefined( player ) || player == level.player )
		return s;
	offset = lane_offset( player );
	if ( side == "far" )
		offset = far_offset( player );
	copy = SpawnStruct();
	copy.origin = s.origin + offset;
	copy.angles = s.angles;
	return copy;
}

// climb_catch is deleted after the first jump; in co-op player 2 still needs its place.
keep_or_delete( ent )
{
	if ( !climb_coop() )
		ent Delete();
}

// --------------------------------------------------------------------------------------------
// Things done once for the whole level, or for every player, where the climb did them once for
// level.player.

// cliff_scene_with_price(): put the players at the bottom of the wall, each in front of his lane.
teleport_to_cave()
{
	if ( !climb_coop() )
	{
		// the patched climb functions work on self, which the original ran as level.player
		level.player thread maps\_climb::teleport_to_cave();
		return;
	}
	foreach ( player in level.players )
		player thread maps\_climb::teleport_to_cave();
}

// self: player, in teleport_to_cave(). Player 2 stands beside player 1, on his lane's side.
cave_offset()
{
	if ( !climb_coop() || self == level.player )
		return ( 0, 0, 0 );
	offset = lane_offset( self );
	side = 40;
	if ( Length( offset ) < side )
		side = Length( offset );
	return VectorNormalize( offset ) * side;
}

death_triggers()
{
	if ( !climb_coop() )
	{
		// on the player: the patched death_trigger scream and Kill() go to self (a thread on the level left single player without either)
		level.player thread maps\_climb::death_trigger();
		return;
	}
	foreach ( player in level.players )
		player thread maps\_climb::death_trigger();
}

give_icepick_ammo()
{
	foreach ( player in level.players )
	{
		if ( player HasWeapon( level.ice_pick_viewweapon ) )
		{
			player GiveMaxAmmo( level.ice_pick_viewweapon );
			player SetWeaponAmmoClip( level.ice_pick_viewweapon, 90 );
		}
	}
}

// player_big_jump(): Soap jumps the gap once, for whoever gets to the ledge first.
price_jump_once()
{
	if ( isdefined( level.mw2coop_price_jump ) )
		return;
	level.mw2coop_price_jump = true;
	thread maps\_climb::price_makes_the_jump( GetEnt( "climb_jump_org", "targetname" ) );
}

// On easy and normal the jump trigger is lowered, once.
lower_jump_trigger_once( trigger )
{
	if ( isdefined( trigger.mw2coop_lowered ) )
		return;
	trigger.mw2coop_lowered = true;
	trigger.origin += ( 0, 0, -40 );
}

// self: player. The climb's arm state, also on the rig for the rig's notetracks.
note_arm_globals( globals, model )
{
	level.arm_ent_globals = globals;
	self.mw2coop_arm_globals = globals;
	model.mw2coop_player = self;
	model.mw2coop_arm_globals = globals;
}

// self: a rig model (climbing arms or the big jump arms). Its player.
rig_player()
{
	if ( isdefined( self.mw2coop_player ) )
		return self.mw2coop_player;
	return level.player;
}

rig_arm_globals()
{
	if ( isdefined( self.mw2coop_arm_globals ) )
		return self.mw2coop_arm_globals;
	return level.arm_ent_globals;
}

// --------------------------------------------------------------------------------------------
// Big jump: Soap catches one player at a time. Notetrack "start_gaz" of a player's big jump
// rig (self: the rig's anim node, rig: the rig). If Soap is still catching the other player,
// this rig holds still where the player hangs on until Soap is free.

catch_queued( rig )
{
	if ( !climb_coop() )
	{
		self maps\_climb::gaz_catches_player( rig );
		return;
	}
	player = rig rig_player();
	// a player who let go after the jump fell (player_big_jump cleared his player_hangs_on); his
	// hidden rig still reaches this notetrack: no catch, or Soap would hold up the other player
	if ( !player pflag( "player_hangs_on" ) )
	{
		maps\_mw2coop::state( "climb: " + player_name( player ) + " fell after the jump, no catch" );
		return;
	}
	if ( isdefined( level.mw2coop_catching ) )
	{
		maps\_mw2coop::state( "climb: " + player_name( player ) + " hangs on, Soap is catching the other player" );
		animation = rig getanim( "big_jump" );
		rig SetFlaggedAnim( "single anim", animation, 1, 0, 0 );
		while ( isdefined( level.mw2coop_catching ) )
			wait 0.05;
		if ( !isdefined( rig ) )
			return;
		rig SetFlaggedAnim( "single anim", animation, 1, 0, 1 );
	}
	level.mw2coop_catching = player;
	maps\_mw2coop::state( "climb: Soap catches " + player_name( player ) );
	self maps\_climb::gaz_catches_player( rig );
}

// player_big_jump(), self: a player who did not hold on after the jump (on Hardened and Veteran
// he must keep a pick button pressed until Soap catches him) and was let go. In single player he
// falls into the map's death volume; in co-op his lane is beside the jump, where he came to rest
// inside the rock (playtest 2026-10-23: stuck there for a minute). He falls to his death at once,
// as in single player: his own fade_to_death (death_trigger: the scream, black, mission failed).
jump_fell()
{
	if ( !climb_coop() )
		return;
	maps\_mw2coop::state( "climb: " + player_name( self ) + " let go after the jump" );
	self pflag_set( "fade_to_death" );
}

// The difficulty the climb plays: in co-op always the easy one (owner, 2026-10-23, after a
// playtest: "как на лёгкой сложности"), whatever the host picked. On easy and normal the climb
// gives ten seconds before the first stab must hold, does not ask for the right pick separately,
// lowers the big jump's trigger (no jump needed), does not let the player fall after the jump,
// and shows the climbing hint sooner. Letting go of both picks on the wall still drops a player,
// as on easy: back to the bottom before the safe point, to his death past it.
climb_skill()
{
	if ( climb_coop() )
		return 0;
	return level.gameSkill;
}

// gaz_catches_player(): the level makes a new Soap (friendly_init_cliffhanger: a fresh spawn) for
// the catch. Once in single player; in co-op Soap catches twice, and the second catch made a
// second Soap (playtest 2026-10-23: two MacTavishes at the top). The second catch reuses him.
soap_for_catch()
{
	if ( climb_coop() && isdefined( level.mw2coop_soap_made ) && isdefined( level.price ) && IsAlive( level.price ) )
		return;
	[[ level.friendly_init_cliffhanger ]]();
	level.mw2coop_soap_made = true;
}

// From gaz_catches_player() once Soap's catch animation is over.
catch_done()
{
	level.mw2coop_catching = undefined;
}

// The player Soap is catching (price_catches_player, slowmo_gaz).
caught_player()
{
	if ( isdefined( level.mw2coop_catching ) )
		return level.mw2coop_catching;
	return level.player;
}

// self: player, at the end of his big jump. The climb clears "can_save" when the player falls to
// his death and then gives up on him; in co-op that is this player's own fall (fade_to_death).
climb_survived()
{
	if ( !climb_coop() )
		return flag( "can_save" );
	return !self ent_flag( "fade_to_death" );
}

// climb_init() creates the dvar climb_startdir as "up" with SetDvarIfUninitialized, but in this
// build GetDvar returns something else when the climb starts (QA log), so the arm animations
// stay empty until the first stab: the getting-on-the-wall animation never played (script
// errors). In co-op any value the climb does not know means "up", as climb_init() meant;
// single player is left as it is.
climb_start_dir()
{
	dir = GetDvar( "climb_startdir" );
	if ( !climb_coop() || dir == "up" || dir == "left" || dir == "right" )
		return dir;
	if ( !isdefined( level.mw2coop_startdir_noted ) )
	{
		level.mw2coop_startdir_noted = true;
		maps\_mw2coop::state( "climb: climb_startdir is '" + dir + "', climbing up" );
	}
	return "up";
}

// --------------------------------------------------------------------------------------------
// What the partner sees of a climber. The climb draws the climber as its first-person arms and
// picks (player_rig, a world model everyone sees) with his body standing beside them: the partner
// saw four arms (owner's playtest, 2026-10-24). The arms are hidden from the partner, the
// climber's own model is taken off, and the partner sees him as Soap climbs: his body's mesh on
// Soap's skeleton (maps\_mw2coop_bodies::rider_model, the snowmobile rider's model), a pick in
// each hand, in Soap's climbing animation, his eyes at the rig's view. The animation runs while
// he goes up and stops while he hangs.

#using_animtree( "generic_human" );

// player_finishes_climbing(), self: the climb's player_rig.
rig_seen_by_partner( climber )
{
	if ( !climb_coop() )
		return;
	other = maps\_mw2coop::other_player( climber );
	if ( !isdefined( other ) || !isdefined( climber.mw2coop_body ) )
		return;
	self hideOnClient( other );
	body = Spawn( "script_model", self GetTagOrigin( "tag_player" ) );
	body SetModel( maps\_mw2coop_bodies::rider_model( climber.mw2coop_body ) );
	body Attach( climber.mw2coop_head, "", true );
	body Attach( "weapon_ice_picker", "tag_inhand" );
	body Attach( "weapon_ice_picker", "tag_weapon_left" );
	body.angles = ( 0, self GetTagAngles( "tag_player" )[ 1 ], 0 );
	body UseAnimTree( #animtree );
	body SetAnimKnobAllRestart( %cliffhanger_climb_up_mid, %body, 1, 0, 0 );
	body SetAnimTime( %cliffhanger_climb_up_mid, climb_anim_from() );
	wait 0.05;
	if ( !isdefined( self ) )
	{
		body Delete();
		return;
	}
	body climb_body_to_eyes( self, climber );
	body hideOnClient( climber );
	climber.mw2coop_climb_body = body;	// QA climbview
	climber.mw2coop_climb_rig = self;	// QA climbview
	climber maps\_utility::SetModelFunc( undefined );
	body thread climb_body_moves( self, climber );
	self waittill( "death" );
	body Delete();
	if ( isdefined( climber ) && IsAlive( climber ) )
		climber maps\_mw2coop_bodies::give_body( maps\_mw2coop::body_model );
}

// The climb shows and hides its arms as it goes (player_relinks_to_tag, player_gets_back_on_wall:
// ent.viewModel Show()); a Show() undoes hideOnClient, and the partner saw the arms again over
// the climber's head (QA climbview frame, 2026-10-24). self: the climb's arms; climber: their
// player.
rig_show( climber )
{
	self Show();
	if ( !climb_coop() || !isdefined( climber ) )
		return;
	other = maps\_mw2coop::other_player( climber );
	if ( isdefined( other ) )
		self hideOnClient( other );
}

// self: the climbing body. His head at the climber's eyes, a little behind them, facing as the
// rig does: Soap's climb moves the body about within the animation (its head drifted 12 to 27
// units from the view, QA climbview), so this is done every frame rather than once by a link.
// The rig's tag_player is not his eyes: it was up to 56 units off the wall from them, and the
// body hung in the air there.
climb_body_to_eyes( rig, climber )
{
	self.angles = ( 0, rig GetTagAngles( "tag_player" )[ 1 ], 0 );
	eyes = climber GetEye() - AnglesToForward( self.angles ) * climb_eyes_back();
	self.origin = eyes - ( self GetTagOrigin( "j_head" ) - self.origin );
}

climb_anim_from()
{
	return 0.3;	// a stretch of Soap's climb up the open wall
}

climb_eyes_back()
{
	return 4;
}

// self: the climbing body; rig: the climb's arms. Soap's climb plays while the climber works his
// picks (a swing button pressed or let go in the last second) or the rig rises, and holds while
// he hangs still (owner: the body's arms stood still while his first-person arms swung). It
// starts over from the same stretch before it reaches the top of Soap's wall.
climb_body_moves( rig, climber )
{
	self endon( "death" );
	rig endon( "death" );
	last = rig.origin[ 2 ];
	rate = 0;
	held = [];
	held[ "left" ] = climber swing_pressed( "left", "mouse1" );
	held[ "right" ] = climber swing_pressed( "right", "mouse2" );
	busy_until = 0;
	for ( ;; )
	{
		wait 0.05;
		left = climber swing_pressed( "left", "mouse1" );
		right = climber swing_pressed( "right", "mouse2" );
		if ( left != held[ "left" ] || right != held[ "right" ] )
			busy_until = GetTime() + 1000;
		held[ "left" ] = left;
		held[ "right" ] = right;
		if ( rig.origin[ 2 ] - last > 0.2 )
			busy_until = GetTime() + 300;
		last = rig.origin[ 2 ];
		want = 0;
		if ( GetTime() < busy_until )
			want = 1;
		if ( want != rate )
		{
			rate = want;
			self SetAnim( %cliffhanger_climb_up_mid, 1, 0.2, rate );
		}
		if ( self GetAnimTime( %cliffhanger_climb_up_mid ) > 0.85 )
			self SetAnimTime( %cliffhanger_climb_up_mid, climb_anim_from() );
		self climb_body_to_eyes( rig, climber );
		self climb_body_overlap( climber );
	}
}

// self: the climbing body. On the second wall both players hang and start at the same spot (the partner waits there for his catch while the climber goes up
// from it): the partner's eyes were inside the climbing body and its arms and picks filled his view ("mantis arms", owner, 2026-11-26; QA climb frame
// final-own: the climber's legs and rifle at the eyes of the partner hanging under him: his eyes 54 units from the line from the body's feet to its head, 97
// below the climber's; Soap's climb throws the legs out). Hidden from the partner while he is under the climber (more than 40 units lower, within 66 of that
// line), shown to him again past 74 or once he is less than 30 lower (1.5 s later the line was 78 away and the body above him looked right). Side by side on the
// first wall the eyes are at about the same height. The climber never sees it. Positions only: the same on both machines.
climb_body_overlap( climber )
{
	other = maps\_mw2coop::other_player( climber );
	if ( !isdefined( other ) )
		return;
	mine = climber GetEye();
	his = other GetEye();
	below = mine[ 2 ] - his[ 2 ];
	d = body_eye_distance( self.origin, mine, his );
	if ( below > 40 && d < 66 && !isdefined( self.mw2coop_overlap_hidden ) )
	{
		self.mw2coop_overlap_hidden = true;
		self Hide();
	}
	else if ( isdefined( self.mw2coop_overlap_hidden ) && ( d > 74 || below < 30 ) )
	{
		self.mw2coop_overlap_hidden = undefined;
		self Show();
		self hideOnClient( climber );
	}
}

// the distance from eye to the line from feet to head (a body's length)
body_eye_distance( feet, head, eye )
{
	along = head - feet;
	len = VectorDot( along, along );
	f = 0;
	if ( len > 0 )
		f = VectorDot( eye - feet, along ) / len;
	if ( f < 0 )
		f = 0;
	if ( f > 1 )
		f = 1;
	return Distance( eye, feet + along * f );
}
