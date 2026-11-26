// mw2coop QA, Cliffhanger ice climb (scenario "climb", default start). maps\_mw2coop_qa starts it
// through level.mw2coop_qa_climb, set by maps\_mw2coop_climb: this file names maps\_climb, which
// exists only in Cliffhanger's zone, so no script loaded on every map may name this file.
#include common_scripts\utility;
#include maps\_utility;
#using_animtree( "generic_human" );

// Cliffhanger ice climb (default start): a bot plays both players. Swings go through the climb's
// co-op input (self.mw2coop_qa_hold, read by maps\_mw2coop_climb::swing_pressed in place of the
// player's attack/aim buttons); walking and running are velocity pushes. Both players must get
// up the wall on their own lanes, make the big jump, be caught by Soap and reach the top.
climb()
{
	maps\_mw2coop_qa::check( level.players.size == 2 && isdefined( level.player2 ), "two players (" + level.players.size + ")" );
	foreach ( player in level.players )
		player thread climb_bot();
	if ( isdefined( level.mw2coop_qa_climb_frames ) )	// (scenario climbfinal: player 1's camera is moved, the "together at the top" check then fails)
		level thread climb_final_frames();
	for ( t = 0; t < 420 && !flag( "reached_top" ); t += 5 )
	{
		maps\_mw2coop_qa::report( "t " + t + " can_save " + flag( "can_save" ) + " | p1 " + climb_text( level.player ) + " | p2 " + climb_text( level.player2 ) );
		wait 5;
	}
	foreach ( player in level.players )
		maps\_mw2coop_qa::check( player ent_flag_exist( "reached_top" ) && player ent_flag( "reached_top" ), maps\_mw2coop_climb::player_name( player ) + " reached the top" );
	foreach ( player in level.players )
		maps\_mw2coop_qa::check( player ent_flag_exist( "player_was_caught" ) && player ent_flag( "player_was_caught" ), "Soap caught " + maps\_mw2coop_climb::player_name( player ) );
	maps\_mw2coop_qa::check( flag( "reached_top" ), "the level goes on (reached_top)" );
	wait 3;
	d = Distance( level.player.origin, level.player2.origin );
	maps\_mw2coop_qa::check( d < 400, "both players together at the top (" + int( d ) + " units)" );
	// climb_leftover_frames();	// (2026-11-26: the climb rigs left on the walls are hidden ones: no frame showed them)
	if ( isdefined( level.mw2coop_qa_climb_real ) )
	{
		more = 0;
		n = 0;
		if ( isdefined( level.mw2coop_qa_climb_presses ) )
			foreach ( p in level.mw2coop_qa_climb_presses )
			{
				n++;
				if ( p > 1 )
					more++;
			}
		if ( isdefined( level.mw2coop_qa_first_grabs ) )
			maps\_mw2coop_qa::report( "real buttons: presses for the first grab, each attempt:" + level.mw2coop_qa_first_grabs );
		maps\_mw2coop_qa::check( n > 0 && more == 0, "real buttons: every stab and grab took one press (" + more + " of " + n + " needed more)" );
	}
	maps\_mw2coop_qa::report( "done" );
}

// Scenario "climbview" (default start): what the players look like with the ice picks. Before the
// climb player 2 draws his picks (frames of both machines: player 1 looking at him, his own
// view); then only player 2 climbs (the bot) while player 1 stays below and looks at him from the
// side and from behind.
climbview()
{
	maps\_mw2coop_qa::check( level.players.size == 2 && isdefined( level.player2 ), "two players (" + level.players.size + ")" );
	wait 3;
	foreach ( player in level.players )
		maps\_mw2coop_qa::report( maps\_mw2coop_climb::player_name( player ) + " holds " + player GetCurrentWeapon() + ", has " + weapons_text( player ) );
	if ( level.player2 HasWeapon( "ice_picker" ) )
		level.player2 SwitchToWeapon( "ice_picker" );
	wait 2.5;
	maps\_mw2coop_qa::report( "player 2 holds " + level.player2 GetCurrentWeapon() );
	look_at( level.player2, 70, 20 );
	maps\_mw2coop_qa::qa_shot( "picks-drawn" );
	look_at( level.player2, 70, 90 );
	maps\_mw2coop_qa::qa_shot( "picks-drawn-side" );
	level.player Unlink();
	level.player2 thread climb_bot();
	wait 6;
	if ( !flag( "price_begins_climbing" ) )
		flag_set( "price_begins_climbing" );
	level.player2 ent_flag_wait( "player_starts_climbing" );
	cam = Spawn( "script_origin", level.player.origin );
	for ( n = 0; n < 3; n++ )
	{
		wait 3;
		body = level.player2.mw2coop_climb_body;
		if ( isdefined( body ) )
		{
			soap = "none";
			if ( isdefined( level.price ) )
				soap = "" + level.price.origin;
			maps\_mw2coop_qa::report( "body " + body.origin + " player 2 " + level.player2.origin + " soap " + soap );
			// hang player 1's view beside the body, at its height: the wall's side, then behind
			aim = body GetTagOrigin( "j_spine4" );
			dir = AnglesToForward( ( 0, body.angles[ 1 ] + 100 + n * 40, 0 ) );
			level.player Unlink();
			cam.origin = aim + dir * 110 - ( 0, 0, 60 );
			level.player SetOrigin( cam.origin );
			level.player PlayerLinkTo( cam );
			wait 0.1;
			level.player SetPlayerAngles( VectorToAngles( aim - level.player GetEye() ) );
			wait 0.7;
		}
		else
			look_from_lane( level.player2, n * 60 );
		if ( isdefined( level.player2.mw2coop_climb_body ) )
		{
			b = level.player2.mw2coop_climb_body;
			maps\_mw2coop_qa::report( "climb body " + b.model + " head " + b GetTagOrigin( "j_head" ) + " eye " + level.player2 GetEye() + " anim time " + b GetAnimTime( %cliffhanger_climb_up_mid ) );
		}
		else
			maps\_mw2coop_qa::report( "no climb body" );
		maps\_mw2coop_qa::qa_shot( "climbing-" + n );
	}
	maps\_mw2coop_qa::report( "done" );
}

// Player 1's view hangs dist away from who's chest, yaw degrees round from his facing, and looks
// at it (linked to a script_origin, so he does not fall).
look_at( who, dist, yaw )
{
	if ( !isdefined( level.mw2coop_qa_cam ) )
		level.mw2coop_qa_cam = Spawn( "script_origin", level.player.origin );
	cam = level.mw2coop_qa_cam;
	aim = who GetEye() - ( 0, 0, 20 );
	dir = AnglesToForward( ( 0, who.angles[ 1 ] + yaw, 0 ) );
	level.player Unlink();
	cam.origin = aim + dir * dist - ( 0, 0, 60 );
	level.player SetOrigin( cam.origin );
	level.player PlayerLinkTo( cam );
	wait 0.1;
	level.player SetPlayerAngles( VectorToAngles( aim - level.player GetEye() ) );
	wait 0.7;
}

// Player 1 at the foot of his own lane (where he would climb beside him), looking at who.
look_from_lane( who, back )
{
	start = level.player.mw2coop_climb_start;
	if ( !isdefined( start ) )
		return;
	away = VectorNormalize( ( start[ 0 ] - who.origin[ 0 ], start[ 1 ] - who.origin[ 1 ], 0 ) );
	level.player SetOrigin( start + away * back );
	level.player SetPlayerAngles( VectorToAngles( who GetEye() - ( 0, 0, 20 ) - level.player GetEye() ) );
	wait 0.8;
}

weapons_text( player )
{
	text = "";
	list = player GetWeaponsList();
	if ( !isdefined( list ) )
		return text;
	foreach ( w in list )
		text += w + " ";
	return text;
}

climb_text( p )
{
	text = int( p.origin[ 0 ] ) + "," + int( p.origin[ 1 ] ) + "," + int( p.origin[ 2 ] ) + " linked " + p IsLinked() + " " + p.mw2coop_qa_step;
	foreach ( name in strtok( "player_in_position_to_climb player_starts_climbing climb_pullup player_preps_for_jump climb_big_jump player_makes_the_jump player_hangs_on player_was_caught final_climb reached_top", " " ) )
		if ( p ent_flag_exist( name ) && p ent_flag( name ) )
			text += " " + name;
	return text;
}

// self: player
climb_bot()
{
	self endon( "death" );
	self.mw2coop_qa_hold = [];
	self.mw2coop_qa_hold[ "left" ] = false;
	self.mw2coop_qa_hold[ "right" ] = false;
	// walk up to Soap, who starts climbing once a player is there (the bot cannot find the way
	// along the ledge: after 6 s the QA sets the trigger's flag itself)
	self.mw2coop_qa_step = "to soap";
	for ( t = 0; t < 6 && !flag( "price_begins_climbing" ) && isdefined( level.price ); t += 0.05 )
	{
		if ( Distance( self.origin, level.price.origin ) > 90 )
			self climb_bot_walk( level.price.origin, 100 );
		wait 0.05;
	}
	if ( !flag( "price_begins_climbing" ) && self == level.player )
	{
		maps\_mw2coop_qa::report( "QA sets price_begins_climbing (bot cannot walk the ledge)" );
		flag_set( "price_begins_climbing" );
	}
	self.mw2coop_qa_step = "wait wall";
	flag_wait( "player_gets_on_wall" );

	// walk to the own lane until in position, then face the wall and swing
	self.mw2coop_qa_step = "walk";
	yaw_check = GetEnt( "player_climb_yaw_check", "targetname" );
	wall = maps\_climb::get_forward_from_ent( yaw_check );
	for ( t = 0; t < 20 && !self ent_flag( "player_in_position_to_climb" ); t += 0.05 )
	{
		self climb_bot_walk( self.mw2coop_climb_start, 120 );
		wait 0.05;
	}
	maps\_mw2coop_qa::report( maps\_mw2coop_climb::player_name( self ) + " in position " + self ent_flag( "player_in_position_to_climb" ) );
	self SetPlayerAngles( VectorToAngles( wall ) );
	self.mw2coop_qa_step = "climb";
	if ( self bot_real() )
	{
		// the first grab: the left pick's button pressed (held) until the climb has begun (the arms are made); presses counted
		self thread climb_real_watch( 6 );
		presses = 0;
		while ( presses < 6 && !isdefined( self.mw2coop_arm_globals ) )
		{
			self bot_hold( "left", false );
			self bot_hold( "left", true );
			presses++;
			for ( t = 0; t < 2.5 && !isdefined( self.mw2coop_arm_globals ); t += 0.05 )
				wait 0.05;
		}
		if ( !isdefined( level.mw2coop_qa_first_grabs ) )
			level.mw2coop_qa_first_grabs = "";
		level.mw2coop_qa_first_grabs += " " + presses;
		wait 0.15;	// (not in the frame the watch reports the climb's start)
		maps\_mw2coop_qa::report( maps\_mw2coop_climb::player_name( self ) + " first grab: " + presses + " press(es)" );	// (reported at the end: a report in the same frame as the watch's was lost)
		if ( !isdefined( level.mw2coop_qa_climb_presses ) )
			level.mw2coop_qa_climb_presses = [];
		level.mw2coop_qa_climb_presses[ level.mw2coop_qa_climb_presses.size ] = presses;
	}
	else
		self bot_hold( "left", true );
	self climb_bot_stabs( "player_preps_for_jump" );

	// over the top of the wall: run and jump the gap
	self.mw2coop_qa_step = "run";
	if ( self == level.player )
	{
		foreach ( name in strtok( "climb_jump_prep climb_big_jump climb_icepick_slide ramp_block_notsolid", " " ) )
			foreach ( trigger in maps\_mw2coop_climb::flag_triggers( name ) )
				maps\_mw2coop_qa::report( "trigger " + name + " " + trigger.classname + " at " + trigger.origin );
		node = GetEnt( "climb_jump_org", "targetname" );
		maps\_mw2coop_qa::report( "climb_jump_org " + node.origin + " " + node.angles + ", me " + self.origin + " " + self GetPlayerAngles() );
	}
	if ( !self bot_real() )	// (real buttons: a player keeps both picks' buttons down over the top and through the jump, ready to hold on)
	{
		self bot_hold( "left", false );
		self bot_hold( "right", false );
	}
	yaw = GetEnt( "big_jump_yaw", "targetname" );
	targ = GetEnt( yaw.target, "targetname" );
	jump = VectorToAngles( targ.origin - yaw.origin );
	jump = ( 0, jump[ 1 ], 0 );
	forward = AnglesToForward( jump );
	maps\_mw2coop_qa::report( "big_jump_yaw " + yaw.origin + " -> " + targ.origin );
	self.mw2coop_qa_hold[ "left" ] = false;
	wait 1;
	maps\_mw2coop_qa::report( maps\_mw2coop_climb::player_name( self ) + " over the top at " + self.origin );
	// to the start of the run-up (big_jump_yaw), player 2 a little behind
	start = yaw.origin;
	if ( self != level.player )
	{
		wait 1.5;
		start -= forward * 40;
	}
	for ( t = 0; t < 8 && Distance2D( self.origin, start ) > 8; t += 0.05 )
	{
		self climb_bot_walk( start, 80 );
		wait 0.05;
	}
	maps\_mw2coop_qa::report( maps\_mw2coop_climb::player_name( self ) + " at the run-up " + self.origin );
	// the run-up is narrow: run along the big_jump_yaw line, steering back onto it
	for ( t = 0; t < 30 && !self ent_flag( "player_makes_the_jump" ); t += 0.05 )
	{
		if ( !self IsLinked() )
		{
			self SetPlayerAngles( jump );
			off = self.origin - yaw.origin;
			off = ( off[ 0 ], off[ 1 ], 0 );
			side = off - forward * VectorDot( off, forward );
			vel = self GetVelocity();
			self SetVelocity( forward * 200 - side * 4 + ( 0, 0, vel[ 2 ] ) );
		}
		wait 0.05;
	}
	maps\_mw2coop_qa::report( maps\_mw2coop_climb::player_name( self ) + " jumped " + self ent_flag( "player_makes_the_jump" ) );

	// hang on until Soap has him, then climb to the top
	self.mw2coop_qa_step = "hang";
	self bot_hold( "left", true );
	self bot_hold( "right", true );
	self ent_flag_wait( "final_climb" );
	self.mw2coop_qa_step = "final";
	self bot_hold( "right", false );
	self climb_bot_stabs( "reached_top" );
	self.mw2coop_qa_step = "top";
	self bot_hold( "left", false );
	self bot_hold( "right", false );
}

// The second climb (after the big jump, Soap's catch): frames of both views while both climb it (final-1..4; owner, 2026-11-26: "mantis arms" on the
// second climb), with what each player's partner rigs are: the climb's arms (player_rig) and the jump's arms (player_arms) shown or not
climb_final_frames()
{
	// player 1 on the second wall: his own view (host frames final-own-<n>; the client's show player 2 waiting for his catch)
	for ( t = 0; t < 200 && !( level.player ent_flag( "final_climb" ) && level.player IsLinked() ); t += 0.25 )
		wait 0.25;
	wait 1;
	for ( n = 1; n <= 2 && level.player IsLinked() && !level.player ent_flag( "reached_top" ); n++ )
	{
		hidden = isdefined( level.player.mw2coop_climb_body ) && isdefined( level.player.mw2coop_climb_body.mw2coop_overlap_hidden );
		text = "final own " + n + ": eyes " + int( Distance( level.player GetEye(), level.player2 GetEye() ) ) + " apart (" + int( Distance2D( level.player GetEye(), level.player2 GetEye() ) ) + " sideways)";
		if ( isdefined( level.player.mw2coop_climb_body ) )
			text += ", player 2's eyes " + int( maps\_mw2coop_climb::body_eye_distance( level.player.mw2coop_climb_body.origin, level.player GetEye(), level.player2 GetEye() ) ) + " from player 1's climbing body";
		maps\_mw2coop_qa::report( text + ", hidden from player 2: " + hidden );
		near = 0;
		foreach ( m in GetEntArray( "script_model", "classname" ) )
			if ( isdefined( m.model ) && Distance( m.origin, level.player2 GetEye() ) < 110 && near < 8 )
			{
				near++;
				maps\_mw2coop_qa::report( "final own " + n + ": near player 2's eyes: " + m.model + " " + int( Distance( m.origin, level.player2 GetEye() ) ) + " units" );
			}
		maps\_mw2coop_qa::qa_shot( "final-own-" + n );
		wait 1.5;
	}
	// player 2 on the second wall while player 1 is up: player 1 looks at him from the sides and from behind (host frames final-look-<n>), player 2's own view
	// (client frames final-look-<n>)
	for ( t = 0; t < 120; t += 0.25 )
	{
		if ( level.player ent_flag( "reached_top" ) && !level.player IsLinked() && level.player2 ent_flag( "final_climb" ) && level.player2 IsLinked() )
			break;
		wait 0.25;
	}
	if ( !level.player ent_flag( "reached_top" ) || !level.player2 IsLinked() || level.player2 ent_flag( "reached_top" ) )
	{
		maps\_mw2coop_qa::report( "final look: no moment with player 1 up and player 2 on the second wall" );
		return;
	}
	yaws = strtok( "90 -90 160 200 120", " " );
	foreach ( n, yaw in yaws )
	{
		if ( level.player2 ent_flag( "reached_top" ) )
			break;
		look_at( level.player2, 90, int( yaw ) );
		text = "final look " + n + " (yaw " + yaw + "): player 2 eye " + vec_text( level.player2 GetEye() );
		if ( isdefined( level.player2.mw2coop_climb_body ) )
			text += " body " + vec_text( level.player2.mw2coop_climb_body.origin ) + " anim time " + level.player2.mw2coop_climb_body GetAnimTime( %cliffhanger_climb_up_mid );
		else
			text += " no climbing body";
		maps\_mw2coop_qa::report( text );
		maps\_mw2coop_qa::qa_shot( "final-look-" + n );
	}
	level.player Unlink();
}

// After the climb: player 1's camera goes to each first-person arms model still in the level (the climb's rigs left on the walls) and looks at it (frames leftover-<n>)
climb_leftover_frames()
{
	models = GetEntArray( "script_model", "classname" );
	n = 0;
	cam = Spawn( "script_origin", level.player.origin );
	foreach ( m in models )
	{
		if ( !isdefined( m ) || !isdefined( m.model ) || !IsSubStr( m.model, "viewhands" ) || n >= 6 )
			continue;
		n++;
		at = m.origin + ( 0, 0, 30 );
		level.player Unlink();
		cam.origin = at + AnglesToForward( ( 0, 165 + 180, 0 ) ) * 110 + ( 0, 0, 20 ) - ( 0, 0, 60 );
		level.player SetOrigin( cam.origin );
		level.player PlayerLinkTo( cam );
		wait 0.1;
		level.player SetPlayerAngles( VectorToAngles( at - level.player GetEye() ) );
		wait 0.6;
		text = "leftover " + n + ": " + m.model + " at " + vec_text( m.origin );
		if ( isdefined( m.targetname ) )
			text += " targetname " + m.targetname;
		if ( isdefined( m.animname ) )
			text += " animname " + m.animname;
		if ( isdefined( m.mw2coop_player ) )
			text += " jump arms of " + maps\_mw2coop_climb::player_name( m.mw2coop_player );
		maps\_mw2coop_qa::report( text );
		maps\_mw2coop_qa::qa_shot( "leftover-" + n );
	}
	level.player Unlink();
	cam Delete();
}

vec_text( v )
{
	return int( v[ 0 ] ) + "," + int( v[ 1 ] ) + "," + int( v[ 2 ] );
}

// QA climbreal / climbreal1: the player whose own machine presses his real attack (left pick) and aim (right pick) buttons through the usercmd (press(): the
// DLL runs the key bindings), as the mouse does; the other player plays by self.mw2coop_qa_hold. One player at a time: both would share the press dvar.
bot_real()
{
	if ( !isdefined( level.mw2coop_qa_climb_real ) )
		return false;
	if ( level.mw2coop_qa_climb_real == 2 )
		return isdefined( level.player2 ) && self == level.player2;
	return self == level.player;
}

arm_button( arm )
{
	if ( arm == "left" )
		return "attack";
	return "speed_throw";
}

// self: player. Holds an arm's button down (a press, kept) or lets it go
bot_hold( arm, on )
{
	if ( !self bot_real() )
	{
		self.mw2coop_qa_hold[ arm ] = on;
		return;
	}
	if ( !isdefined( self.mw2coop_qa_real ) )
		self.mw2coop_qa_real = [];
	held = isdefined( self.mw2coop_qa_real[ arm ] ) && self.mw2coop_qa_real[ arm ];
	if ( on )
		maps\_mw2coop_qa::press( self, arm_button( arm ), 600000 );
	else if ( held )
		maps\_mw2coop_qa::press( self, arm_button( arm ), 1 );	// (a held button: the press is no new one, the release lets it go)
	self.mw2coop_qa_real[ arm ] = on;
}

// QA climbreal: for <seconds>, each change of what the climb reads: the player's attack / aim buttons from his usercmd, the arm the climb waits for, and each
// arm's press count, the count it last used and whether it counts as held (pressed within 750 ms)
climb_real_watch( seconds )
{
	self endon( "death" );
	last = "";
	for ( t = 0; t < seconds; t += 0.05 )
	{
		s = "atk " + self AttackButtonPressed() + " ads " + self AdsButtonPressed();
		g = self.mw2coop_arm_globals;
		if ( isdefined( g ) && isdefined( g.arm_ents ) )
		{
			s += " cur " + g.current_arm;
			foreach ( arm in strtok( "left right", " " ) )
			{
				e = g.arm_ents[ arm ];
				if ( !isdefined( e ) || !isdefined( e.button_press_num ) )
					continue;
				s += " | " + arm + " " + e.button_press_num + "/" + e.button_press_num_last + " held " + ( e.last_button_press + 750 > GetTime() );
			}
		}
		if ( s != last )
			maps\_mw2coop_qa::report( "watch " + maps\_mw2coop_climb::player_name( self ) + " t " + t + ": " + s );
		last = s;
		wait 0.05;
	}
}

// self: climbing player. Swing arm after arm (press the arm to swing while holding the other)
// until the flag is set.
climb_bot_stabs( until )
{
	stabs = 0;
	while ( !self ent_flag( until ) )
	{
		g = self.mw2coop_arm_globals;
		if ( !isdefined( g ) || !self IsLinked() )
		{
			wait 0.05;
			continue;
		}
		arm = g.current_arm;
		other = maps\_climb::get_other_arm( arm );
		if ( self bot_real() )
		{
			// a player's way: the other pick's button held, this one's let go and pressed again (held); presses counted until the arm changes
			if ( !isdefined( self.mw2coop_qa_real ) || !isdefined( self.mw2coop_qa_real[ other ] ) || !self.mw2coop_qa_real[ other ] )
				self bot_hold( other, true );
			// a press counts when this arm's stab begins within 2 s (maps\_climb::arm_completes_stab notes the press count it starts from: mw2coop_press_at_stab);
			// then the bot waits for the stab to end (the arm changes) without pressing, as a player who saw his pick move
			ent = g.arm_ents[ arm ];
			before = ent.mw2coop_press_at_stab;
			presses = 0;
			started = false;
			latency = -1;
			while ( presses < 6 && !started && isdefined( self.mw2coop_arm_globals ) && self.mw2coop_arm_globals == g && g.current_arm == arm && !self ent_flag( until ) )
			{
				self bot_hold( arm, false );
				pressed_at = GetTime();
				self bot_hold( arm, true );
				presses++;
				for ( t = 0; t < 2 && !started && g.current_arm == arm && !self ent_flag( until ); t += 0.05 )
				{
					started = isdefined( ent.mw2coop_press_at_stab ) && ( !isdefined( before ) || ent.mw2coop_press_at_stab != before );
					if ( started )
						latency = GetTime() - pressed_at;
					else
						wait 0.05;
				}
				if ( g.current_arm != arm )
					started = true; // (a stab begun and ended between two looks)
			}
			for ( t = 0; t < 6 && isdefined( self.mw2coop_arm_globals ) && self.mw2coop_arm_globals == g && g.current_arm == arm && !self ent_flag( until ); t += 0.05 )
				wait 0.05;
			stabs++;
			maps\_mw2coop_qa::report( maps\_mw2coop_climb::player_name( self ) + " " + until + " stab " + stabs + " (" + arm + "): " + presses + " press(es), began " + latency + " ms after the last" );
			if ( !isdefined( level.mw2coop_qa_climb_presses ) )
				level.mw2coop_qa_climb_presses = [];
			level.mw2coop_qa_climb_presses[ level.mw2coop_qa_climb_presses.size ] = presses;
			continue;
		}
		self.mw2coop_qa_hold[ other ] = true;
		self.mw2coop_qa_hold[ arm ] = false;
		wait 0.2;
		self.mw2coop_qa_hold[ arm ] = true;
		for ( t = 0; t < 3 && isdefined( self.mw2coop_arm_globals ) && self.mw2coop_arm_globals == g && g.current_arm == arm && !self ent_flag( until ); t += 0.05 )
			wait 0.05;
	}
}

// self: player. One frame of walking towards target (a velocity push, height kept); stops at an
// edge (no floor within 40 units below a step ahead).
climb_bot_walk( target, speed )
{
	to = target - self.origin;
	to = VectorNormalize( ( to[ 0 ], to[ 1 ], 0 ) );
	ahead = self.origin + to * 24;
	floor = PlayerPhysicsTrace( ahead + ( 0, 0, 24 ), ahead - ( 0, 0, 40 ) );
	vel = self GetVelocity();
	if ( floor[ 2 ] < self.origin[ 2 ] - 36 )
	{
		self SetVelocity( ( 0, 0, vel[ 2 ] ) );
		return false;
	}
	self SetVelocity( to * speed + ( 0, 0, vel[ 2 ] ) );
	return true;
}

// Scenario "climblb" (start "climb"): the Nexus report on 1.0.1 (couch co-op, pads): "in Cliffhanger, as soon as you start to climb the ice it crashes, in the exact
// moment I press LB". Each player walks to his lane, holds the pad's right trigger (the left pick: the climb starts), and presses LB (the special grenade) the moment
// he is on the wall; player 1 first, then player 2. A crash ends the run (the log's CRASH line); otherwise the state is reported.
climblb()
{
	maps\_mw2coop_qa::check( level.players.size == 2 && isdefined( level.player2 ), "two players (" + level.players.size + ")" );
	if ( !flag( "price_begins_climbing" ) )
		flag_set( "price_begins_climbing" );
	flag_wait( "player_gets_on_wall" );
	yaw_check = GetEnt( "player_climb_yaw_check", "targetname" );
	wall = maps\_climb::get_forward_from_ent( yaw_check );
	foreach ( n, player in level.players )
	{
		pad = "mw2coop_qa_pad" + ( n + 1 );
		for ( t = 0; t < 20 && !player ent_flag( "player_in_position_to_climb" ); t += 0.05 )
		{
			player climb_bot_walk( player.mw2coop_climb_start, 120 );
			wait 0.05;
		}
		player SetPlayerAngles( VectorToAngles( wall ) );
		maps\_mw2coop_qa::report( "climblb: " + maps\_mw2coop_climb::player_name( player ) + " in position " + player ent_flag( "player_in_position_to_climb" ) + ", weapon " + player GetCurrentWeapon() );
		if ( GetDvar( "mw2coop_qa_lbfirst" ) == "1" ) // (LB first, as if it were the left pick; then LB held with the trigger)
		{
			SetDvar( pad, "0 0 0 0 256 0 0 0" );
			wait 0.6;
			SetDvar( pad, "0 0 0 0 0 0 0 0" );
			wait 0.3;
			SetDvar( pad, "0 0 0 0 256 0 255 0" );
			wait 0.6;
		}
		SetDvar( pad, "0 0 0 0 0 0 255 0" );	// the right trigger held: the left pick
		for ( t = 0; t < 8 && !player IsLinked(); t += 0.05 )
			wait 0.05;
		maps\_mw2coop_qa::report( "climblb: " + maps\_mw2coop_climb::player_name( player ) + " linked " + player IsLinked() + " after " + t + " s, weapon " + player GetCurrentWeapon() );
		SetDvar( pad, "0 0 0 0 256 0 255 0" );	// + LB
		wait 0.3;
		SetDvar( pad, "0 0 0 0 0 0 255 0" );
		wait 3;
		maps\_mw2coop_qa::report( "climblb: " + maps\_mw2coop_climb::player_name( player ) + " 3 s after LB: alive " + IsAlive( player ) + ", linked " + player IsLinked() + ", weapon " + player GetCurrentWeapon() );
		SetDvar( pad, "0 0 0 0 0 0 0 0" );
		wait 2;
	}
	// both on the wall, both looking at it: the composed frame (each half: his own arms and picks, the partner at his side)
	SetDvar( "mw2coop_qa_frame", "climb-both" );
	wait 1;
	maps\_mw2coop_qa::report( "done" );
}
