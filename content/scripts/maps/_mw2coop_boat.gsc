// mw2coop: the Endgame boat chase (af_chase) for two players. Called from the patches of
// maps/af_chase_code.gsc and maps/af_chase_waterfall.gsc (tools/gen_boat_patches.py). Those
// scripts are byte-identical in the af_chase and ending zones, so this file names only them and
// the common scripts. Outside a co-op campaign session every entry point does what the original
// code did.
//
// Owner's design (2026-10-21): whoever mounts the boat first drives it (the level's player,
// level.mw2coop_lead, every level.player of the chase); the other player rides in the boat as a
// gunner with his own weapons and normal damage. Nobody can revive anyone in the boat, so a
// player going down there is the single player death: the boat flips and the mission fails.
#include common_scripts\utility;
#include maps\_utility;
#using_animtree( "generic_human" );

coop()
{
	return maps\_mw2coop::is_campaign_coop() && isdefined( level.player2 );
}

// price_position_switch() and set_price_auto_switch_pose(): in co-op Price stays on his tube
// (the right one, his "left" pose) and the passenger keeps the other: two gunners, a side each.
price_keeps_tube()
{
	return coop();
}

player_name( player )
{
	if ( player == level.player )
		return "player 1";
	return "player 2";
}

// --------------------------------------------------------------------------------------------
// Boarding

// trigger_boat_mount(), self: the trigger at the jetty: the player who set it off drives.
boat_mount( who )
{
	if ( !coop() || flag( "player_on_boat" ) || !isdefined( who ) || !IsPlayer( who ) )
		return;
	level.mw2coop_lead = who;
}

// setup_boat_for_drive(), once the driver is on the boat (also the start points' UseBy): the
// other player lies at the bow as the second gunner. (The driver's stance cannot be changed: the
// boat already forbids him to crouch, and no stance left drops the session, "All stances
// disallowed for linked player".)
boat_boarded()
{
	if ( !coop() )
		return;
	driver = level.mw2coop_lead;
	passenger = maps\_mw2coop::other_player( driver );
	maps\_mw2coop::state( "boat: " + player_name( driver ) + " drives" );
	// QA drives through this (maps\_mw2coop_qa names no chase script)
	level.mw2coop_qa_boat_autopilot = ::qa_autopilot;
	if ( isdefined( passenger ) && IsAlive( passenger ) )
		passenger thread passenger_ride( level.players_boat );
}

// self: the passenger. He lies at the bow as Price does, with Price's own boat animations
// (maps\_zodiac_ai): Price's "left" pose lies on the right tube and aims ahead and across to the
// left, "right" is the mirror. In co-op Price keeps his "left" pose (price_keeps_tube) and the
// passenger takes the other, so each covers a side; should anything still turn Price over, the
// passenger turns too. A player's body cannot play those animations, so his own model is
// removed and a body plays them (attached at tag_guy2, as Price is); his view is at its eyes,
// it aims where he looks, and he looks as far as the pose aims (owner, 2026-10-21: "the same
// position as Price, both lying, on opposite sides"; his view limited). The body only shows: it
// has no collision, and his shots and the hits on him are his player's, whose eyes are its
// eyes. Both lockstep machines pose it from the same input (his view angles).
passenger_ride( boat )
{
	self endon( "death" );
	level endon( "mw2coop_level_end" );
	boat endon( "death" );
	pose = other_pose( price_pose() );
	body = Spawn( "script_model", boat GetTagOrigin( "tag_guy2" ) );
	body.angles = boat GetTagAngles( "tag_guy2" );
	// Price's boat body (body_desert_tf141_zodiac, the level's TF141 desert uniform) with the
	// player's own head: the boat animations are made for that skeleton. On the player's
	// multiplayer body (coop_body_*) they bent the upper body forward and down, head 30 units
	// low, and the body looked thrown about (QA boat, same pose as Price: Price's model matched
	// him to 2 units, the co-op model did not).
	body SetModel( level.price.model );
	if ( isdefined( self.mw2coop_head ) )
		body Attach( self.mw2coop_head, "", true );
	body UseAnimTree( #animtree );
	// at the tag as Price: the poses themselves lie him on his tube, the mirror of Price's
	body LinkTo( boat, "tag_guy2", ( 0, 0, 0 ), ( 0, 0, 0 ) );
	level.mw2coop_boat_body = body;
	body thread body_holds_weapon( self );
	body body_pose( pose );
	// his own player model goes: linked to the seat it played its in-air animation and hung
	// over the bow (driver's screenshots, 2026-10-21); neither Hide nor hideOnClient hides a
	// co-op player. The game's own way: maps\_loadout::UpdateModel() with no model function.
	// The body is hidden from himself, as the game does for a dog attack's drone
	// (animscripts\dog\dog_combat PlayerView_Show)
	self maps\_utility::SetModelFunc( undefined );
	body hideOnClient( self );
	// without his model his name floated far ahead of the boat on the driver's screen: no names
	// in the boat (the DLL draws them at size 0, as in the knife fight)
	SetDvar( "mw2coop_hide_names", "1" );
	wait 0.4;	// the pose blends in over 0.2 s; his view goes to its eyes once it lies

	seat = spawn_tag_origin();
	seat.origin = eye_seat( body );
	seat.angles = ( 0, boat.angles[ 1 ] + pose_center( pose ), 0 );
	seat LinkTo( boat );
	level.mw2coop_boat_seat = seat;
	self SetStance( "crouch" );
	self AllowStand( false );
	self AllowProne( false );
	self EnableWeapons();
	// he starts looking ahead over the bow, as Price does when he has nobody to shoot at
	self SetPlayerAngles( ( 0, boat.angles[ 1 ], 0 ) );
	// he looks only as far as the body can aim (maps\_zodiac_ai: 48 to 51 degrees either way
	// of the pose), so the body's gun always points where his shots go; the poses do not aim up
	// or down, so little of that
	self PlayerLinkTo( seat, "tag_origin", 0.8, 48, 48, 20, 15 );
	self thread body_aims( body, boat );
	maps\_mw2coop::state( "boat: " + player_name( self ) + " lies at the bow in the " + pose + " pose (" + seat_text( boat, seat.origin ) + "; Price in the " + price_pose() + " pose)" );
	for ( ;; )
	{
		wait 0.25;
		want = other_pose( price_pose() );
		if ( want == pose || isdefined( level.mw2coop_boat_over_edge ) )
			continue;
		// turn over with Price's own transition; the view rides along at the body's eyes
		trans = anim.boatanims[ pose ].trans;
		body.mw2coop_turning = true;
		body SetFlaggedAnimKnobAllRestart( "trans", trans, %body, 1, 0.2 );
		from = AngleClamp180( seat.angles[ 1 ] - boat.angles[ 1 ] );
		to = pose_center( want );
		length = GetAnimLength( trans ) - 0.3;
		for ( t = 0.05; t <= length; t += 0.05 )
		{
			seat Unlink();
			seat.origin = eye_seat( body );
			seat.angles = ( 0, boat.angles[ 1 ] + from + ( to - from ) * ( t / length ), 0 );
			seat LinkTo( boat );
			wait 0.05;
		}
		pose = want;
		body body_pose( pose );
		body.mw2coop_turning = undefined;
		wait 0.4;
		seat Unlink();
		seat.origin = eye_seat( body );
		seat.angles = ( 0, boat.angles[ 1 ] + to, 0 );
		seat LinkTo( boat );
		maps\_mw2coop::state( "boat: Price turned over, " + player_name( self ) + " too (" + pose + " pose)" );
	}
}

price_pose()
{
	if ( isdefined( level.price ) && isdefined( level.price.a ) && isdefined( level.price.a.boat_pose ) )
		return level.price.a.boat_pose;
	return "left";	// maps\_zodiac_ai's pose for most of the chase
}

other_pose( pose )
{
	if ( pose == "left" )
		return "right";
	return "left";
}

// The boat-relative yaw a pose aims along, as a view angle (to the left positive).
// maps\_zodiac_ai::getDesiredBoatAimYaw() has 40.5 and -36 the other way round: the AI's yaws
// (animscripts\utility::GetYawToSpot()) are to the right positive. (Taking them as view angles
// put the passenger's view 37 degrees left while his body's gun pointed 35 right.)
pose_center( pose )
{
	if ( pose == "left" )
		return 40.5;
	return -36;
}

// self: the passenger's body. A pose's aim, centred.
body_pose( pose )
{
	anims = anim.boatanims[ pose ];
	self.mw2coop_pose = pose;
	// as maps\_zodiac_ai::think() after a pose change: the aim centre alone up to the root (a
	// transition left on underneath stood the body up), then the pose's base
	self SetAnimKnobAllRestart( anims.aim.center, %body, 1, 0.2 );
	self SetAnimKnobAll( anims.base, %zodiac_actions, 1, 0.2 );
	// and the pose's idle, as maps\_zodiac_ai::enableBoatIdle() keeps on Price while he aims
	self SetAnimKnob( anims.idle, 1, 0.2 );
}

// Where the passenger's player is linked so that his crouched eyes are just in front of the
// body's face (the view at tag_eye itself would show the inside of its head); its arms and gun
// are where his own first-person gun is, which is drawn over them.
eye_seat( body )
{
	return body GetTagOrigin( "tag_eye" ) + AnglesToForward( body GetTagAngles( "tag_eye" ) ) * 6 - ( 0, 0, 40 );
}

// self: the passenger. His body aims where he looks, as Price's aims at his target
// (maps\_zodiac_ai::updateBoatAim()).
body_aims( body, boat )
{
	self endon( "death" );
	body endon( "death" );
	boat endon( "death" );
	for ( ;; )
	{
		wait 0.05;
		if ( isdefined( body.mw2coop_turning ) )
			continue;
		anims = anim.boatanims[ body.mw2coop_pose ];
		// the AI's aim yaw: from the pose's centre, to the right positive
		aim = AngleClamp180( boat.angles[ 1 ] + pose_center( body.mw2coop_pose ) - self GetPlayerAngles()[ 1 ] );
		if ( aim < 0 )
		{
			frac = min( aim / anims.leftAimLimit, 1 );
			body SetAnimKnob( anims.aim.center, 1 - frac, 0.1 );
			body SetAnim( anims.aim.left, frac, 0.1 );
		}
		else
		{
			frac = min( aim / anims.rightAimLimit, 1 );
			body SetAnimKnob( anims.aim.center, 1 - frac, 0.1 );
			body SetAnim( anims.aim.right, frac, 0.1 );
		}
		// as updateBoatAim() does after every aim blend: the pose's base stays on
		body SetAnimKnobAll( anims.base, %zodiac_actions, 1, 0.2 );
	}
}

// self: the passenger's body. It holds his current weapon.
body_holds_weapon( player )
{
	self endon( "death" );
	player endon( "death" );
	held = "";
	for ( ;; )
	{
		weapon = player GetCurrentWeapon();
		if ( weapon != held && weapon != "none" )
		{
			if ( held != "" )
				self Detach( GetWeaponModel( held ), "tag_weapon_right" );
			self Attach( GetWeaponModel( weapon ), "tag_weapon_right" );
			held = weapon;
		}
		wait 0.25;
	}
}

// The passenger's own body again (the boat flipped or is gone).
passenger_body_back( p )
{
	if ( isdefined( level.mw2coop_boat_body ) )
		level.mw2coop_boat_body Delete();
	if ( isdefined( p.mw2coop_head ) )
		p maps\_mw2coop_bodies::give_body( maps\_mw2coop::body_model );
	SetDvar( "mw2coop_hide_names", "" );
}

seat_text( boat, at )
{
	d = at - boat.origin;
	return "seat " + int( VectorDot( d, AnglesToForward( boat.angles ) ) ) + " " + int( VectorDot( d, AnglesToRight( boat.angles ) ) ) + " " + int( VectorDot( d, AnglesToUp( boat.angles ) ) ) + " from the boat";
}

passenger()
{
	if ( !coop() )
		return undefined;
	return maps\_mw2coop::other_player( level.mw2coop_lead );
}

// --------------------------------------------------------------------------------------------
// Death in the boat

// flip_when_player_dies(), in place of the player's death: either player going down (or dying).
boat_rider_down()
{
	if ( !coop() )
	{
		level.player waittill( "death" );
		return;
	}
	for ( ;; )
	{
		foreach ( player in level.players )
		{
			if ( !IsAlive( player ) || maps\_mw2coop::is_downed( player ) )
			{
				maps\_mw2coop::state( "boat: " + player_name( player ) + " is down in the boat, it flips" );
				level.mw2coop_boat_down = player;
				return;
			}
		}
		wait 0.05;
	}
}

// flip_when_player_dies(), self: the boat, with the driver on the rolling camera: the passenger
// rolls with him.
boat_flip_partner( offset_obj )
{
	p = passenger();
	if ( !isdefined( p ) || !IsAlive( p ) )
		return;
	passenger_body_back( p );
	p Unlink();
	p PlayerLinkToDelta( offset_obj, "tag_player", 1.0, 0, 0, 0, 0 );
	p PlayerSetGroundReferenceEnt( offset_obj );
}

// flip_when_player_dies(), once the boat has rolled over: in single player the player is dead and
// the game ends the mission; here nobody is.
boat_flip_done()
{
	if ( !coop() )
		return;
	wait 1;
	level thread maps\_mw2coop::mission_failed();
}

// --------------------------------------------------------------------------------------------
// Over the waterfall

// water_fall_edge(), as the boat tips over the edge: no shooting any more.
passenger_over_edge()
{
	p = passenger();
	if ( !isdefined( p ) || !IsAlive( p ) )
		return;
	level.mw2coop_boat_over_edge = true;
	p DisableWeapons();
	maps\_mw2coop::state( "boat: over the waterfall" );
}

// water_fall_edge(), when the boat is gone and the driver sinks under the water at linkent: the
// passenger sinks beside him and gets the same water on his screen.
passenger_underwater( linkent )
{
	p = passenger();
	if ( !isdefined( p ) || !IsAlive( p ) )
		return;
	right = AnglesToRight( linkent.angles );
	spot = spawn_tag_origin();
	spot.origin = linkent.origin + right * 40;
	spot.angles = linkent.angles;
	spot LinkTo( linkent );
	passenger_body_back( p );
	p Unlink();
	p PlayerLinkToDelta( spot, "tag_origin", 1, 0, 0, 0, 0 );
	p PlayRumbleOnEntity( "damage_heavy" );
	p SetWaterSheeting( 3, 3 );
}

// --------------------------------------------------------------------------------------------
// QA

// The bot cannot steer: the boat follows the river's spline for the given time, as
// rapids_scene() steers it.
qa_autopilot( seconds )
{
	boat = level.players_boat;
	boat endon( "death" );
	for ( t = 0; t < seconds; t += 0.1 )
	{
		// ahead on the river's spline (the driver's place on it is tracked as level.player's)
		if ( isdefined( level.mw2coop_lead.targ ) )
		{
			targ = maps\_vehicle_spline_zodiac::get_player_targ();
			progress = maps\_vehicle_spline_zodiac::get_player_progress();
			goal = maps\_vehicle_spline_zodiac::get_position_from_spline_unlimited( targ, progress + 700, 0 );
			boat VehicleDriveTo( ( goal[ 0 ], goal[ 1 ], boat.origin[ 2 ] ), 75 );
		}
		else if ( isdefined( level.enemy_boat ) )
			boat VehicleDriveTo( level.enemy_boat.origin, 75 );
		wait 0.1;
	}
}
