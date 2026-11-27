// mw2coop: The Gulag for two players. Called from mw2coop/patches/maps/gulag_code.gsc.patch and
// gulag.gsc.patch; outside a co-op campaign session every entry point does nothing.
//
// Fly-in: player 1 rides the helicopter on a hidden view controller turret at tag_guy2
// (spawn_player_heli). Player 2 spawned at the map's player start, which hangs over nothing:
// he fell 67000 units in the spawn audit. He now gets his own view controller at the same door,
// side by side with player 1 as COD-MOD seats them (tag_guy2, player 1 20 units one way, player 2
// 10 units the other), looks where player 1 is made to look, and gets off next to him after the
// landing. A seat at the other door (QA tour 2026-10-22) had him look at the cabin's floor.
#include common_scripts\utility;
#include maps\_utility;
#include maps\_anim;

// Where a player's view controller sits on tag_guy2 (player 1: 1, player 2: 2); player 1 alone
// keeps the level's own place.
door_offset( n )
{
	if ( !maps\_mw2coop::is_campaign_coop() || !isdefined( level.player2 ) )
		return ( 0, 0, -8 );
	if ( n == 1 )
		return ( 20, 0, -8 );
	return ( -10, 0, -8 );
}

// spawn_player_heli(), right after player 1 is linked to his view controller.
partner_rides_heli()
{
	if ( !maps\_mw2coop::is_campaign_coop() || !isdefined( level.player2 ) )
		return;
	heli = level.player_heli;
	controller = get_player_view_controller( heli, "tag_guy2", door_offset( 2 ) );
	if ( isdefined( level.view_org ) )
		controller SetTargetEntity( level.view_org );
	level.mw2coop_view_controller2 = controller;

	p2 = level.player2;
	p2 SetOrigin( heli GetTagOrigin( "tag_guy2" ) );
	p2 AllowProne( false );
	p2 AllowCrouch( false );
	p2 AllowSprint( false );
	p2 AllowJump( false );
	if ( isdefined( level.ground_ref ) )
		p2 PlayerSetGroundReferenceEnt( level.ground_ref );
	// the same view limits as player 1 (spawn_player_heli)
	p2 PlayerLinkToDelta( controller, "TAG_aim", 1, -35, 40, -35, 40, true );
	maps\_mw2coop::state( "gulag: player 2 rides the helicopter at player 1's door" );
	SetDvar( "mw2coop_ride", heli GetEntityNumber() ); // (the DLL holds both bodies to it, as the level's men: src/native.c, pass2_hook)
	level thread partner_view_follows_landing( controller );
}

// The fly-in turns player 1's view controller to the gulag centre for the landing.
partner_view_follows_landing( controller )
{
	controller endon( "death" );
	flag_wait( "player_goes_in_for_landing" );
	center = GetEnt( "gulag_center", "targetname" );
	if ( !isdefined( center ) )
		return;
	controller ClearTargetEntity();
	controller SetTargetEntity( center );
}

// gulag.gsc, after the landing deleted player 1's view controller.
partner_leaves_heli()
{
	if ( !maps\_mw2coop::is_campaign_coop() || !isdefined( level.player2 ) )
		return;
	p2 = level.player2;
	SetDvar( "mw2coop_ride", "" );
	// Out of the door the way player 1 is (gulag_perimeter: his view controller is moved 32 units out along its aim, then deleted): player 2 was unlinked at his door, inside the
	// helicopter's collision, and could not move (Nexus report, 1.0.1: "my friend couldn't get off the helicopter in the Gulag"); the teleport next to player 1 below found no free spot
	// beside a player still standing at the cabin door.
	controller = level.mw2coop_view_controller2;
	if ( isdefined( controller ) )
	{
		angles = controller GetTagAngles( "tag_aim" );
		forward = AnglesToForward( ( 0, angles[ 1 ], 0 ) ) * 32;
		ent = spawn_tag_origin();
		ent.origin = controller.origin;
		ent.angles = controller.angles;
		controller LinkTo( ent );
		ent MoveTo( ent.origin + forward + ( 0, 0, 4.5 ), 0.2, 0.1, 0.1 );
		wait( 0.2 );
		controller Delete();
		ent Delete();
	}
	if ( p2 IsLinked() )
		p2 Unlink();
	p2 PlayerSetGroundReferenceEnt( undefined );
	p2 AllowProne( true );
	p2 AllowCrouch( true );
	p2 AllowSprint( true );
	p2 AllowJump( true );
	if ( maps\_mw2coop::spot_stuck( p2.origin ) ) // still wedged in the cabin: next to player 1 once he stands on free ground (up to 30 s)
		p2 thread maps\_mw2coop::teleport_to_partner_when_safe( level.player, 30 );
	maps\_mw2coop::state( "gulag: player 2 off the helicopter" );
}

// --------------------------------------------------------------------------------------------
// The cave-in chase out of the tunnels (gulag_ending_code.gsc): the collapse follows both players
// and buries whichever one it catches. Player 1's screen darkens on the level's black_overlay,
// player 2's on his own.

// The buried player: player 1 unless the chase caught the other one.
cavein_default()
{
	if ( !isdefined( level.mw2coop_cavein_player ) )
		level.mw2coop_cavein_player = level.player;
}

// The collapse is at org, player 1 chase_dist from it (flat). True when it reaches a player, who
// is then the one buried.
cavein_catches( org, chase_dist, reach )
{
	if ( chase_dist < reach )
	{
		level.mw2coop_cavein_player = level.player;
		return true;
	}
	if ( !maps\_mw2coop::is_campaign_coop() || !isdefined( level.player2 ) || !IsAlive( level.player2 ) )
		return false;
	if ( Distance2D( level.player2.origin, org ) >= reach )
		return false;
	level.mw2coop_cavein_player = level.player2;
	return true;
}

// A player in the volume that buries whoever runs the wrong way.
anyone_touching( volume )
{
	foreach ( player in level.players )
	{
		if ( player != level.player && !maps\_mw2coop::is_campaign_coop() )
			continue;
		if ( player IsTouching( volume ) )
		{
			level.mw2coop_cavein_player = player;
			return true;
		}
	}
	return false;
}

// With the level's black screen: player 2's own (on), or gone (off).
partner_black_screen( on )
{
	if ( isdefined( level.mw2coop_chase_black ) )
		level.mw2coop_chase_black Destroy();
	level.mw2coop_chase_black = undefined;
	if ( !on || !maps\_mw2coop::is_campaign_coop() || !isdefined( level.player2 ) )
		return;
	hud = maps\_hud_util::create_client_overlay( "black", 0, level.player2 );
	hud.alpha = 1;
	hud.foreground = false;
	level.mw2coop_chase_black = hud;
}

// Player 2's screen darkens by his own distance to the collapse at org, as player 1's does.
partner_chase_fade( org, max_black, black_range )
{
	if ( !isdefined( level.mw2coop_chase_black ) )
		return;
	alpha = 1 - ( ( Distance2D( level.player2.origin, org ) - max_black ) / black_range );
	level.mw2coop_chase_black.alpha = clamp( alpha, 0, 1 );
	level.mw2coop_chase_black FadeOverTime( 0.2 );
}

// --------------------------------------------------------------------------------------------
// The cellblock rappel (gulag_code.gsc player_rappels, cellblock_rappel_player): the level's
// player alone held the use button at the rope, rode the rope's animation with his own rig and
// came off at the bottom; player 2 stayed at the top (owner, 2026-10-24: "the rope: there may be
// problems like with the ice pick"). Now whoever uses the rope first rappels, and the other uses it
// after him: the same rope plays its animation again for him (rappel_rope_again), with a rig of
// his own. The level goes on as soon as the first is on the rope, as it did.

// player_rappels(): who goes down this time. Before co-op's second rappel the rope is not usable.
rappel_wait( trigger, flag_name )
{
	if ( !maps\_mw2coop::is_campaign_coop() || !isdefined( level.player2 ) )
	{
		flag_wait( flag_name );
		return level.player;
	}
	for ( ;; )
	{
		trigger waittill( "trigger", who );
		if ( !isdefined( who ) || !IsPlayer( who ) || isdefined( who.mw2coop_rappelled ) || isdefined( level.mw2coop_rappelling ) )
			continue;
		who.mw2coop_rappelled = true;
		level.mw2coop_rappelling = who;
		flag_set( flag_name );
		return who;
	}
}

// player_rappels(), where it deleted the use trigger. self: the trigger. who: this rappel's player.
rappel_trigger_used( who )
{
	if ( !maps\_mw2coop::is_campaign_coop() || !isdefined( level.player2 ) )
	{
		self Delete();
		return;
	}
	// the rope stays usable for the other player once the first is down
	self trigger_off();
	other = maps\_mw2coop::other_player( who );
	if ( isdefined( other.mw2coop_rappelled ) )
		self Delete();
}

// player_rappels(), where it deleted the rope's glowing outline. self: the outline. The first
// player only hides it: the other player's rope needs it again.
rappel_obj_done( who )
{
	if ( !maps\_mw2coop::is_campaign_coop() || !isdefined( level.player2 ) )
	{
		self Delete();
		return;
	}
	other = maps\_mw2coop::other_player( who );
	if ( isdefined( other.mw2coop_rappelled ) )
		self Delete();
	else
		self Hide();
}

// cellblock_rappel_player(), after the first player is on the rope. self: level. When his scene is
// over the rope hangs ready again and the other player uses it, as the first did.
rappel_partner( ent )
{
	if ( !maps\_mw2coop::is_campaign_coop() || !isdefined( level.player2 ) )
		return;
	level endon( "mw2coop_level_end" );
	wait ent.unlink_time + 6;	// the scene is over: the first player is down
	level.mw2coop_rappelling = undefined;
	ent.rope_ent anim_first_frame_solo( ent.rope, ent.scene );
	if ( isdefined( ent.rope_obj ) )
		ent.rope_obj Show();
	trigger = GetEnt( "rappel_trigger", "script_noteworthy" );
	if ( !isdefined( trigger ) )
		return;
	trigger trigger_on();
	maps\_mw2coop::state( "gulag: the rope is free for the second player" );
	maps\gulag_code::player_rappels( ent );
}

// --------------------------------------------------------------------------------------------
// The fall in the cafeteria (gulag_ending_code.gsc stumble_trigger_think after "player_falls_down"; night audit 2026-11-17): the ceiling comes
// down, player 1 is knocked over (his view tilted by a roller, turned to where he looks), shielded from death, slowed to a quarter, left at 5 health
// with a growing blur, put prone, and the screen goes black for a second before the evacuation. Player 2 stood on unhurt at full speed. Now he
// falls the same way, his own roller turned to where he looks; gulag_ending.gsc's evacuation gives both their speed, view and blur back.

fall_coop()
{
	return maps\_mw2coop::is_campaign_coop() && isdefined( level.player2 ) && IsAlive( level.player2 );
}

// After player 1 is hit (roller: the level's eq_view_roller, already turned to player 1's view; blur_func: the level's player_blur_manager, run on
// player 2: this file is loaded with gulag_code.gsc too, where gulag_ending_code.gsc may not be).
fall_partner_hit( roller, blur_func )
{
	if ( !fall_coop() )
		return;
	p2 = level.player2;
	angles = p2 GetPlayerAngles();
	if ( !isdefined( level.mw2coop_fall ) )
	{
		// his own roller, turned to where he looks, and the ground reference on it
		f = SpawnStruct();
		f.roller = spawn_tag_origin();
		f.roller.origin = roller.origin;
		f.roller.angles = ( 0, angles[ 1 ], 0 );
		f.ref = spawn_tag_origin();
		f.ref.origin = f.roller.origin;
		f.ref.angles = f.roller.angles;
		f.ref LinkTo( f.roller );
		level.mw2coop_fall = f;
	}
	f = level.mw2coop_fall;
	f.yaw = angles[ 1 ] - roller.angles[ 1 ];	// his fall is player 1's turned by the difference of their views
	p2 PlayerSetGroundReferenceEnt( f.ref );
	p2 EnableDeathShield( true );
	p2 SetMoveSpeedScale( 0.25 );
	above = p2.origin + ( 0, 0, 100 ) + AnglesToForward( f.roller.angles ) * 50;
	mult = 1;
	if ( isdefined( p2.damageMultiplier ) && p2.damageMultiplier > 0 )
		mult = p2.damageMultiplier;
	if ( p2.health > 5 )
		p2 DoDamage( ( p2.health - 5 ) / mult, above );
	p2 thread [[ blur_func ]]();
	maps\_mw2coop::state( "gulag cafeteria: player 2 falls with player 1 (health " + p2.health + ")" );
}

// eq_view_roller RotateTo() of the fall: player 2's roller turns the same way in his own frame.
fall_rotate( roller, angles, time, accel, decel )
{
	roller RotateTo( angles, time, accel, decel );
	if ( !fall_coop() || !isdefined( level.mw2coop_fall ) )
		return;
	level.mw2coop_fall.roller RotateTo( angles + ( 0, level.mw2coop_fall.yaw, 0 ), time, accel, decel );
}

// The black second after the fall, on player 2's screen too.
fall_partner_black( on )
{
	if ( !fall_coop() )
		return;
	if ( on )
	{
		if ( !isdefined( level.mw2coop_fall_black ) )
			level.mw2coop_fall_black = maps\_hud_util::create_client_overlay( "black", 0, level.player2 );
		level.mw2coop_fall_black.alpha = 1;
		return;
	}
	if ( isdefined( level.mw2coop_fall_black ) )
		level.mw2coop_fall_black Destroy();
}

// --------------------------------------------------------------------------------------------
// The cellblock rope as the other player sees it: the same "second hands" as Just Like Old Times' rope (maps\_mw2coop_cave::partner_view, owner
// 2026-11-17). The level's rig is first-person hands (viewhands_player_udt), shown for the whole descent next to the rappeller's own upright body.
// Now the rig is hidden from the other player and he sees a stand-in instead: the rappeller's body mesh on a SEAL soldier's skeleton
// (maps\_mw2coop_bodies::rider_model) with his head, playing the soldiers' own rappel from the same window (generic "rappel_start",
// %gulag_rappel_soldier) at the rig's moment, its eyes kept at the rappeller's camera; his own body is off until the rig goes.
#using_animtree( "generic_human" );

// player_rappels(), once the rig's scene runs. who: the rappeller.
rope_partner_view( who, rig )
{
	if ( !maps\_mw2coop::is_campaign_coop() || !isdefined( level.player2 ) || !isdefined( who.mw2coop_body ) || !isdefined( who.mw2coop_head ) )
		return;
	other = maps\_mw2coop::other_player( who );
	if ( !isdefined( other ) )
		return;
	soldier = level.scr_anim[ "generic" ][ "rappel_start" ];
	hands = level.scr_anim[ "player_rappel" ][ "rappel_start" ];
	body = Spawn( "script_model", who.origin );
	body SetModel( maps\_mw2coop_bodies::rider_model( who.mw2coop_body, maps\_mw2coop_bodies::rope_skeleton() ) );
	body Attach( who.mw2coop_head, "", true );
	body hideOnClient( who );
	body DontCastShadows();
	body UseAnimTree( #animtree );
	body SetAnim( soldier, 1, 0, 0 );
	level.mw2coop_rope_body = body;	// QA
	who maps\_utility::SetModelFunc( undefined );
	maps\_mw2coop::state( "gulag: the rappeller seen by the other player as his body on a SEAL soldier's skeleton" );
	while ( isdefined( rig ) && IsAlive( who ) )
	{
		rig hideOnClient( other );	// (again every frame, as in the caves: a Show would clear it)
		body SetAnimTime( soldier, min( rig GetAnimTime( hands ), 0.999 ) );
		body.angles = ( 0, rig GetTagAngles( "tag_player" )[ 1 ], 0 );
		at = body GetTagOrigin( "tag_eye" );
		if ( !isdefined( at ) )
			at = body GetTagOrigin( "j_head" );
		body.origin = body.origin + ( who GetEye() - at );
		wait 0.05;
	}
	body Delete();
	level.mw2coop_rope_body = undefined;
	if ( isdefined( who ) && IsAlive( who ) )
		who maps\_mw2coop_bodies::give_body( maps\_mw2coop::body_model );
}

// Price's cell (gulag_ending_code.gsc.patch hunk 3, from player_gets_knocked_out_by_price): Price knocks player 1 down and holds him under his gun; player 1's
// view is on the scene's rig at the floor (his eye 9 units over it), and his origin is 50 units under the floor. His co-op body stood there, sunk to the
// chest, his rifle in its hands and his name over it (owner's screenshots of player 2's view, 2026-11-27). While he is held, player 2 sees a stand-in
// instead: his body on the SEAL soldier's skeleton (the rope's stand-in model), lying on its back propped on an elbow (the wounded's dying_back_idle),
// its head where his eye is, on the floor; his own body and the name are hidden. QA pricecell.
knocked_down_view( rig )
{
	who = level.player;
	if ( !maps\_mw2coop::is_campaign_coop() || !isdefined( level.player2 ) || !isdefined( who.mw2coop_body ) || !isdefined( who.mw2coop_head ) )
		return;
	for ( t = 0; t < 1 && !who IsLinked(); t += 0.05 )
		wait 0.05;
	body = Spawn( "script_model", who.origin );
	body SetModel( maps\_mw2coop_bodies::rider_model( who.mw2coop_body, maps\_mw2coop_bodies::rope_skeleton() ) );
	body Attach( who.mw2coop_head, "", true );
	body hideOnClient( who );
	body UseAnimTree( #animtree );
	body SetAnim( %dying_back_idle, 1, 0, 1 );
	level.mw2coop_knocked_body = body;	// QA
	who maps\_utility::SetModelFunc( undefined );
	names = GetDvar( "mw2coop_hide_names" ) != "1";
	if ( names )
		SetDvar( "mw2coop_hide_names", "1" );
	maps\_mw2coop::state( "gulag: player 1 knocked down by Price, seen by player 2 as a stand-in lying on the floor" );
	while ( isdefined( rig ) && IsAlive( who ) && who IsLinked() )
	{
		eye = who GetEye();
		body.angles = ( 0, rig GetTagAngles( "tag_player" )[ 1 ], 0 );	// (the scene's way, not his look: the body does not turn with his mouse)
		floor = BulletTrace( eye + ( 0, 0, 8 ), eye - ( 0, 0, 96 ), false, who )[ "position" ];
		head = body GetTagOrigin( "j_head" );
		if ( isdefined( head ) )
			body.origin = ( body.origin[ 0 ] + eye[ 0 ] - head[ 0 ], body.origin[ 1 ] + eye[ 1 ] - head[ 1 ], floor[ 2 ] );
		wait 0.05;
	}
	body Delete();
	level.mw2coop_knocked_body = undefined;
	if ( names )
		SetDvar( "mw2coop_hide_names", "" );
	if ( isdefined( who ) && IsAlive( who ) )
		who maps\_mw2coop_bodies::give_body( maps\_mw2coop::body_model );
	maps\_mw2coop::state( "gulag: player 1 up again after Price's scene, his body back" );
}
