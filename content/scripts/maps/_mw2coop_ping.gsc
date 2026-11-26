// Co-op markers ("pings", owner 2026-11-23; the model: Apex Legends' ping and its wheel). The marker key ([input] ping_key, Z by default; a pad's Back, src/ping.c):
//
// A short press marks what the player looks at, for both players, with a line on the screen and the squad's own voice where the game has a line for it (else a
// short sound; see voice_line). What is under the cross hair decides:
//   a live grenade or a claymore near the cross hair (8 degrees)        -> "grenade!" / "mine!", red, the grenade icon, follows it until it is gone
//   an enemy under the cross hair, or within 4 degrees of it and in sight -> "enemy!", red, follows him, 7 s or until he dies
//   a vehicle: an enemy one (its team axis or team3) / any other          -> "enemy vehicle!" red / "vehicle" in the player's colour, follows it
//   a turret: an enemy at it / nobody or a friend                        -> "enemy gunner!" red / "turret here" in the player's colour
//   a barrel, a car, a gas tank that explodes (not yet exploded)          -> "explosive!", red, the burning barrel or car icon
//   a weapon or an ammunition cache lying where he looks (64 units)       -> "<weapon> here" / "ammo here", yellow, the ammunition icon, until picked up
//   anything else                                                          -> "here", his colour at the spot, 12 s
// Pressed again within 0.4 s: his place marker becomes "danger" (red, 10 s). Pressed while looking at his own place or danger marker: it goes away ("never
// mind"). Each player keeps one place marker, up to 3 enemy markers, 2 item markers and 2 danger markers (the oldest goes); 5 presses in 3 s at most.
//
// Held 0.25 s, the wheel opens round the cross hair (for that player only): enemy, attacking, going here, wait here, danger, fall back, watching, covering
// (clockwise from the top). The mouse or the pad's right stick picks one while the view stays (src/ping.c holds it); letting go places the marker picked where he
// looks, or none when the cursor is back in the middle. "Enemy", "attacking" and "danger" take the enemy, the vehicle or the explosive under the cross hair as the
// press does, else the spot.
//
// The events: in network co-op the notify "menuresponse" ("-1", "mw2coop_ping" / "mw2coop_ping_open" / "mw2coop_ping_sel_<n|x>" / "mw2coop_ping_close_<n|x>") on the
// player, which both PCs' scripts get in the same frame (a client command in the lockstep frames), so both make the same marker; in the native split screen the dvar
// mw2coop_ping_q<0|1> with that player's last events ("<n> <verb> <arg>,...").
// The icons are the game's own: the co-op partner markers (maps\_coop precaches them on every level), the ammunition waypoint and the death screen's grenade,
// barrel and car icons (maps\_load).
#include common_scripts\utility;
#include maps\_utility;

// maps\_mw2coop::main(), before any wait: the wheel's texts and its boxes
ping_precache()
{
	PrecacheShader( "white" );
	PrecacheString( &"MW2COOP_PING_W_ENEMY" );
	PrecacheString( &"MW2COOP_PING_W_ATTACK" );
	PrecacheString( &"MW2COOP_PING_W_GO" );
	PrecacheString( &"MW2COOP_PING_W_WAIT" );
	PrecacheString( &"MW2COOP_PING_W_DANGER" );
	PrecacheString( &"MW2COOP_PING_W_FALLBACK" );
	PrecacheString( &"MW2COOP_PING_W_WATCH" );
	PrecacheString( &"MW2COOP_PING_W_COVER" );
	PrecacheString( &"MW2COOP_PING_W_NEEDAMMO" );
	PrecacheString( &"MW2COOP_PING_W_COME" );
	PrecacheString( &"MW2COOP_PING_W_HINT" );
}

init()
{
	if ( GetDvar( "coop" ) != "1" )
		return;
	for ( t = 0; t < 600 && ( !isdefined( level.players ) || level.players.size < 2 ); t++ )
		wait 0.05;
	if ( !isdefined( level.players ) || level.players.size < 2 )
		return;
	foreach ( player in level.players )
	{
		player.mw2coop_pings = [];
		player.mw2coop_ping_times = [];
		player thread listen();
	}
	if ( getdvarint( "mw2coop_native_split" ) )
		level thread split_listen();
}

listen()
{
	for ( ;; )
	{
		self waittill( "menuresponse", menu, response );
		if ( response.size < 12 || GetSubStr( response, 0, 12 ) != "mw2coop_ping" )
			continue;
		parts = StrTok( GetSubStr( response, 12 ), "_" );
		verb = "tap";
		arg = -1;
		if ( parts.size > 0 )
			verb = parts[ 0 ];
		if ( parts.size > 1 && parts[ 1 ] != "x" )
			arg = int( parts[ 1 ] );
		self event( verb, arg );
	}
}

// the split screen: each player's dvar keeps his last events ("<n> <verb> <arg>,..."); the ones with a number not seen yet are taken, in order
split_listen()
{
	last = [];
	for ( i = 0; i < 2; i++ )
		last[ i ] = queue_last( GetDvar( "mw2coop_ping_q" + i ) );	// (events of an earlier level are not taken)
	for ( ;; )
	{
		wait 0.05;
		for ( i = 0; i < 2; i++ )
		{
			v = GetDvar( "mw2coop_ping_q" + i );
			if ( v == "" )
				continue;
			player = level.player;
			if ( i == 1 )
				player = level.player2;
			list_e = StrTok( v, "," );
			foreach ( e in list_e )
			{
				p = StrTok( e, " " );
				if ( p.size < 3 || int( p[ 0 ] ) <= last[ i ] )
					continue;
				last[ i ] = int( p[ 0 ] );
				if ( isdefined( player ) )
					player event( p[ 1 ], int( p[ 2 ] ) );
			}
		}
	}
}

queue_last( v )
{
	n = 0;
	list_e = StrTok( v, "," );
	foreach ( e in list_e )
	{
		p = StrTok( e, " " );
		if ( p.size && int( p[ 0 ] ) > n )
			n = int( p[ 0 ] );
	}
	return n;
}

event( verb, arg )
{
	if ( !isdefined( self.mw2coop_pings ) )
		return;
	if ( verb == "tap" )
		self press();
	else if ( verb == "open" )
		self wheel_open();
	else if ( verb == "sel" )
		self wheel_sel( arg );
	else if ( verb == "close" )
		self wheel_close( arg );
}

// 5 presses in 3 s at most (a held key's repeats, a spammer); true when this one counts
spam_ok()
{
	now = GetTime();
	recent = [];
	foreach ( t in self.mw2coop_ping_times )
		if ( now - t < 3000 )
			recent[ recent.size ] = t;
	self.mw2coop_ping_times = recent;
	if ( recent.size >= 5 )
		return false;
	self.mw2coop_ping_times[ self.mw2coop_ping_times.size ] = now;
	return true;
}

press()
{
	if ( !self spam_ok() )
		return;
	now = GetTime();
	eye = self GetEye();
	dir = AnglesToForward( self GetPlayerAngles() );

	// pressed twice: the place marker he just made is danger
	last = self.mw2coop_ping_last;
	if ( isdefined( last ) && isdefined( last.huds ) && last.kind == "place" && now - last.made < 400 )
	{
		pos = last.pos;
		spot = last.spot;
		last remove();
		self make( "danger", undefined, pos, undefined );
		self.mw2coop_pings[ self.mw2coop_pings.size - 1 ].spot = spot;
		return;
	}
	trace = BulletTrace( eye, eye + dir * 15000, true, self );
	// looking at his own place or danger marker: it goes away (the spot he looks at is near the marker's: within 48 units, or 3 degrees far away)
	foreach ( ping in self.mw2coop_pings )
		if ( !isdefined( ping.ent ) && isdefined( ping.spot ) )
		{
			near = Distance( trace[ "position" ], ping.spot ) < 48 || VectorDot( VectorNormalize( ping.spot - eye ), dir ) > 0.9985;
			if ( near )
			{
				kind = ping.kind;
				ping remove();
				self say( "cancel", undefined );
				log_line( "ping cancel: player " + player_n( self ) + " " + kind + ", level time " + GetTime() );
				return;
			}
		}

	target = self target_at( eye, dir, trace );
	if ( isdefined( target ) )
	{
		self make( target.kind, target.ent, target.pos, target.title );
		return;
	}
	self make_spot( "place", trace[ "position" ] );
}

// a marker at the spot he looks at, lifted 8 units (the spot itself is kept: the cancel compares with it)
make_spot( kind, spot )
{
	self make( kind, undefined, spot + ( 0, 0, 8 ), undefined );
	self.mw2coop_pings[ self.mw2coop_pings.size - 1 ].spot = spot;
}

// what the cross hair is on, by the order of the list at the top; undefined: nothing to name (a place)
target_at( eye, dir, trace )
{
	ent = trace[ "entity" ];
	t = self aimed_grenade( eye, dir );
	if ( isdefined( t ) )
		return t;
	if ( isdefined( ent ) && is_enemy( ent ) )
		return target( "enemy", ent, ent.origin, undefined );
	ai = self aimed_enemy( eye, dir );
	if ( isdefined( ai ) )
		return target( "enemy", ai, ai.origin, undefined );
	if ( isdefined( ent ) )
	{
		t = self classify( ent );
		if ( isdefined( t ) )
			return t;
	}
	t = self aimed_vehicle( eye, dir );
	if ( isdefined( t ) )
		return t;
	return item_near( trace[ "position" ] );
}

target( kind, ent, pos, title )
{
	t = SpawnStruct();
	t.kind = kind;
	t.ent = ent;
	t.pos = pos;
	t.title = title;
	return t;
}

is_enemy( ent )
{
	return IsAI( ent ) && IsAlive( ent ) && isdefined( ent.team ) && ent.team != "allies" && ent.team != "neutral";
}

// a vehicle that still drives or flies (a wreck keeps the code class script_vehicle and gets the class name script_vehicle_corpse: QA pingtest 2026-11-23)
is_vehicle( ent )
{
	if ( !isdefined( ent.code_classname ) || ent.code_classname != "script_vehicle" )
		return false;
	return !isdefined( ent.classname ) || ent.classname != "script_vehicle_corpse";
}

hostile_team( team )
{
	return isdefined( team ) && ( team == "axis" || team == "team3" );
}

// a vehicle, a turret, something that explodes or a weapon the trace hit
classify( ent )
{
	if ( is_vehicle( ent ) )
	{
		if ( hostile_team( ent.script_team ) )
			return target( "enemy_vehicle", ent, ent.origin, undefined );
		if ( Distance( ent.origin, self.origin ) > 200 )	// (not the one he rides or stands by)
			return target( "vehicle", ent, ent.origin, undefined );
		return undefined;
	}
	if ( isdefined( ent.code_classname ) && ent.code_classname == "misc_turret" )
	{
		owner = ent GetTurretOwner();
		if ( isdefined( owner ) && is_enemy( owner ) )
			return target( "enemy_turret", ent, ent.origin, undefined );
		if ( !isdefined( owner ) || IsPlayer( owner ) || !hostile_team( ent.script_team ) )
			return target( "turret", ent, ent.origin, undefined );
		return undefined;
	}
	kind = explosive_kind( ent );
	if ( isdefined( kind ) )
		return target( kind, ent, ent.origin, undefined );
	if ( isdefined( ent.classname ) && ent.classname.size > 7 && GetSubStr( ent.classname, 0, 7 ) == "weapon_" )
		return target( "weapon", ent, ent.origin, weapon_title( ent.classname ) );
	return undefined;
}

// "explosive_barrel" / "explosive_car" / "explosive" for a barrel (maps\_interactive_objects), a car or a gas tank (common_scripts\_destructible) that has not
// exploded yet; undefined for anything else
explosive_kind( ent )
{
	if ( isdefined( ent.exploded ) )
		return undefined;
	if ( ( isdefined( ent.targetname ) && ent.targetname == "explodable_barrel" ) || ( isdefined( ent.script_noteworthy ) && ent.script_noteworthy == "explodable_barrel" ) )
		return "explosive_barrel";
	if ( !isdefined( ent.destructible_type ) )
		return undefined;
	type = ent.destructible_type;
	if ( type.size > 8 && GetSubStr( type, 0, 8 ) == "vehicle_" )
		return "explosive_car";
	if ( IsSubStr( type, "propane_tank" ) || IsSubStr( type, "oxygen_tank" ) || IsSubStr( type, "gaspump" ) || IsSubStr( type, "transformer" ) )
		return "explosive";
	return undefined;
}

// a live grenade or claymore within 8 degrees of the cross hair, 1500 units at most, in sight; the nearest the cross hair
aimed_grenade( eye, dir )
{
	best = undefined;
	best_dot = 0.990;
	list_g = GetEntArray( "grenade", "classname" );
	foreach ( g in list_g )
	{
		if ( !isdefined( g ) || Distance( g.origin, eye ) > 1500 )
			continue;
		at = g.origin + ( 0, 0, 4 );
		d = VectorDot( VectorNormalize( at - eye ), dir );
		if ( d > best_dot && SightTracePassed( eye, at, false, self ) )
		{
			best = g;
			best_dot = d;
		}
	}
	if ( !isdefined( best ) )
		return undefined;
	kind = "grenade";
	if ( isdefined( best.model ) && IsSubStr( best.model, "claymore" ) )
		kind = "mine";
	return target( kind, best, best.origin, undefined );
}

// the enemy nearest the cross hair within 4 degrees that the player can see (the cross hair need not be on him exactly)
aimed_enemy( eye, dir )
{
	best = undefined;
	best_dot = 0.9976;
	list_ai = GetAIArray();
	foreach ( ai in list_ai )
	{
		if ( !is_enemy( ai ) )
			continue;
		at = ai GetEye();
		d = VectorDot( VectorNormalize( at - eye ), dir );
		if ( d > best_dot && SightTracePassed( eye, at, false, self ) )
		{
			best = ai;
			best_dot = d;
		}
	}
	return best;
}

// a vehicle the cross hair points at though the trace missed it (a helicopter far away is small; a car's body may stop the trace without naming the car)
aimed_vehicle( eye, dir )
{
	// the cross hair's line passes within 110 units of the vehicle's middle (an angle would miss a near car: aimed at its roof, 400 units away, the middle is 6 degrees
	// off), 8000 units at most, and the line to the middle is clear (or ends on the vehicle's body)
	best = undefined;
	best_off = 110;
	list_v = Vehicle_GetArray();
	foreach ( v in list_v )
	{
		if ( !isdefined( v ) || !is_vehicle( v ) || Distance( v.origin, self.origin ) < 200 )
			continue;
		c = v.origin + ( 0, 0, 40 );
		along = VectorDot( c - eye, dir );
		if ( along <= 0 || along > 8000 )
			continue;
		off = Distance( eye + dir * along, c );
		if ( off >= best_off )
			continue;
		tr = BulletTrace( eye, c, false, self );
		if ( tr[ "fraction" ] > 0.97 || ( isdefined( tr[ "entity" ] ) && tr[ "entity" ] == v ) || Distance( tr[ "position" ], c ) < 140 )
		{
			best = v;
			best_off = off;
		}
	}
	if ( !isdefined( best ) )
		return undefined;
	if ( hostile_team( best.script_team ) )
		return target( "enemy_vehicle", best, best.origin, undefined );
	return target( "vehicle", best, best.origin, undefined );
}

// a weapon lying within 64 units of the spot, or an ammunition cache within 80 (the trace passes through a lying weapon)
item_near( spot )
{
	best = undefined;
	best_d = 64;
	list_e = GetEntArray();
	foreach ( e in list_e )
	{
		if ( !isdefined( e.classname ) )
			continue;
		reach = 0;
		if ( e.classname.size > 7 && GetSubStr( e.classname, 0, 7 ) == "weapon_" )
			reach = 64;
		else if ( e.classname == "script_model_pickup_claymore" || ( isdefined( e.targetname ) && e.targetname == "ammo_cache" ) )
			reach = 80;
		if ( !reach )
			continue;
		d = Distance( e.origin, spot );
		if ( d < reach && ( !isdefined( best ) || d < best_d ) )
		{
			best = e;
			best_d = d;
		}
	}
	if ( !isdefined( best ) )
		return undefined;
	if ( isdefined( best.targetname ) && best.targetname == "ammo_cache" )
		return target( "ammo", best, best.origin, undefined );
	if ( best.classname == "script_model_pickup_claymore" )
		return target( "weapon", best, best.origin, "Claymore" );
	return target( "weapon", best, best.origin, weapon_title( best.classname ) );
}

starts( s, p )
{
	return s.size >= p.size && GetSubStr( s, 0, p.size ) == p;
}

// the weapon's name as the game shows it, from its file name ("weapon_scar_h_reflex" -> "SCAR-H"); the names are the same in every language
weapon_title( name )
{
	if ( starts( name, "weapon_" ) )
		name = GetSubStr( name, 7 );
	if ( starts( name, "m40a3" ) ) return "M40A3";
	if ( starts( name, "m203_m4" ) || starts( name, "m4" ) ) return "M4A1";
	if ( starts( name, "m1014" ) ) return "M1014";
	if ( starts( name, "m14" ) ) return "M14 EBR";
	if ( starts( name, "m16" ) ) return "M16A4";
	if ( starts( name, "m21" ) ) return "M21 EBR";
	if ( starts( name, "m240" ) ) return "M240";
	if ( starts( name, "m79" ) ) return "Thumper";
	if ( starts( name, "beretta393" ) ) return "M93 Raffica";
	if ( starts( name, "beretta" ) ) return "M9";
	if ( starts( name, "usp" ) ) return "USP .45";
	if ( starts( name, "deserteagle" ) ) return "Desert Eagle";
	if ( starts( name, "coltanaconda" ) ) return ".44 Magnum";
	if ( starts( name, "colt45" ) ) return "M1911";
	if ( starts( name, "glock" ) || starts( name, "g18" ) ) return "G18";
	if ( starts( name, "pp2000" ) ) return "PP2000";
	if ( starts( name, "tmp" ) ) return "TMP";
	if ( starts( name, "mp5k" ) ) return "MP5K";
	if ( starts( name, "mp5" ) ) return "MP5";
	if ( starts( name, "ump45" ) ) return "UMP45";
	if ( starts( name, "kriss" ) ) return "Vector";
	if ( starts( name, "p90" ) ) return "P90";
	if ( starts( name, "uzi" ) ) return "Mini-Uzi";
	if ( starts( name, "ak74u" ) ) return "AK-74u";
	if ( starts( name, "ak47" ) ) return "AK-47";
	if ( starts( name, "masada" ) ) return "ACR";
	if ( starts( name, "scar_h" ) ) return "SCAR-H";
	if ( starts( name, "tavor" ) ) return "TAR-21";
	if ( starts( name, "fn2000" ) ) return "F2000";
	if ( starts( name, "famas" ) ) return "FAMAS";
	if ( starts( name, "fal" ) ) return "FAL";
	if ( starts( name, "g36c" ) ) return "G36C";
	if ( starts( name, "g3" ) ) return "G3";
	if ( starts( name, "aug" ) ) return "AUG HBAR";
	if ( starts( name, "sa80" ) ) return "L86 LSW";
	if ( starts( name, "rpd" ) ) return "RPD";
	if ( starts( name, "mg4" ) ) return "MG4";
	if ( starts( name, "saw" ) ) return "M249 SAW";
	if ( starts( name, "barrett" ) ) return "Barrett .50cal";
	if ( starts( name, "cheytac" ) ) return "Intervention";
	if ( starts( name, "wa2000" ) ) return "WA2000";
	if ( starts( name, "dragunov" ) ) return "Dragunov";
	if ( starts( name, "remington700" ) ) return "R700";
	if ( starts( name, "spas12" ) ) return "SPAS-12";
	if ( starts( name, "aa12" ) ) return "AA-12";
	if ( starts( name, "striker" ) ) return "Striker";
	if ( starts( name, "model1887" ) ) return "Model 1887";
	if ( starts( name, "ranger" ) ) return "Ranger";
	if ( starts( name, "winchester1200" ) ) return "W1200";
	if ( starts( name, "rpg" ) ) return "RPG-7";
	if ( starts( name, "at4" ) ) return "AT4-HS";
	if ( starts( name, "stinger" ) ) return "Stinger";
	if ( starts( name, "javelin" ) ) return "Javelin";
	if ( starts( name, "riotshield" ) ) return "Riot Shield";
	if ( starts( name, "claymore" ) ) return "Claymore";
	if ( starts( name, "c4" ) ) return "C4";
	if ( starts( name, "mp44" ) ) return "MP44";
	if ( starts( name, "skorpion" ) ) return "Skorpion";
	return name;
}

// the marker's group (its limit): what it follows decides, a spot is the place group
group_of( kind, ent )
{
	if ( !isdefined( ent ) )
		return "place";
	if ( kind == "enemy" || kind == "enemy_vehicle" || kind == "enemy_turret" || kind == "attack" )
		return "enemy";
	if ( kind == "weapon" || kind == "ammo" || kind == "vehicle" || kind == "turret" || kind == "needammo" )
		return "item";
	return "danger";
}

group_limit( group )
{
	if ( group == "enemy" )
		return 3;
	if ( group == "place" )
		return 1;
	return 2;
}

is_red( kind )
{
	return kind == "enemy" || kind == "enemy_vehicle" || kind == "enemy_turret" || kind == "danger" || kind == "grenade" || kind == "mine" || IsSubStr( kind, "explosive" );
}

shader_of( kind )
{
	if ( kind == "grenade" || kind == "mine" )
		return "hud_grenadeicon";
	if ( kind == "explosive_barrel" )
		return "hud_burningbarrelicon";
	if ( kind == "explosive_car" )
		return "hud_burningcaricon";
	if ( kind == "weapon" || kind == "ammo" || kind == "needammo" )
		return "waypoint_ammo";
	if ( is_red( kind ) || kind == "attack" || kind == "explosive" )
		return "coop_player_location_fire";
	return "coop_player_location";
}

color_of( kind, player )
{
	if ( is_red( kind ) )
		return ( 1, 0.25, 0.2 );
	if ( kind == "attack" )
		return ( 1, 0.55, 0.1 );
	if ( kind == "weapon" || kind == "ammo" || kind == "needammo" )
		return ( 1, 1, 0.3 );
	if ( isdefined( level.player2 ) && player == level.player2 )
		return ( 0.635, 0.929, 0.604 );	// Bravo's (the game's co-op partner green)
	return ( 0.4, 0.75, 1 );	// Alpha's
}

// where the marker sits above what it follows: a man's head, a vehicle's roof, a turret's top (an entity of its own, linked, which the HUD element follows:
// SetTargetEnt puts the icon at the origin of its entity, a man's feet)
lift_of( kind, ent )
{
	if ( IsAI( ent ) || IsPlayer( ent ) )
		return 78;
	if ( is_vehicle( ent ) )
		return 72;
	if ( kind == "turret" || kind == "enemy_turret" )
		return 32;
	return 0;
}

make( kind, ent, pos, title )
{
	// the same entity marked again: one marker
	if ( isdefined( ent ) )
		foreach ( ping in self.mw2coop_pings )
			if ( isdefined( ping.ent ) && ping.ent == ent )
			{
				ping remove();
				break;
			}
	// over the limit the oldest of the group goes
	group = group_of( kind, ent );
	for ( ;; )
	{
		same = [];
		foreach ( ping in self.mw2coop_pings )
			if ( ping.group == group )
				same[ same.size ] = ping;
		if ( same.size < group_limit( group ) )
			break;
		same[ 0 ] remove();
	}

	ping = SpawnStruct();
	ping.kind = kind;
	ping.group = group;
	ping.owner = self;
	ping.ent = ent;
	ping.pos = pos;
	ping.made = GetTime();
	ping.ai = isdefined( ent ) && IsAI( ent );	// (a killed man's entity is a corpse, no longer AI: his marker goes by IsAlive)
	ping.huds = [];
	// what the icon follows: the grenade itself (it flies), an anchor linked above a man, a vehicle or a turret, or nothing (a fixed spot above a barrel, a car,
	// a lying weapon)
	follow = undefined;
	if ( isdefined( ent ) )
	{
		lift = lift_of( kind, ent );
		if ( kind == "grenade" || kind == "mine" )
			follow = ent;
		else if ( lift > 0 )
		{
			ping.anchor = Spawn( "script_model", ent.origin + ( 0, 0, lift ) );
			ping.anchor SetModel( "tag_origin" );
			ping.anchor LinkTo( ent );
			follow = ping.anchor;
		}
		else if ( IsSubStr( kind, "explosive" ) )
			pos = ent.origin + ( 0, 0, 48 );
		else
			pos = ent.origin + ( 0, 0, 16 );
		ping.pos = pos;
	}
	shader = shader_of( kind );
	color = color_of( kind, self );
	foreach ( viewer in level.players )
	{
		if ( kind == "needammo" && viewer == self )
			continue;	// (a marker over his own head is for the partner only)
		h = NewClientHudElem( viewer );
		h SetShader( shader, 1, 1 );
		h.color = color;
		h.alpha = 0.95;
		h.hidewheninmenu = true;
		if ( isdefined( follow ) )
		{
			h SetWayPoint( true, true );
			h SetTargetEnt( follow );
		}
		else
		{
			h.x = pos[ 0 ];
			h.y = pos[ 1 ];
			h.z = pos[ 2 ];
			h SetWayPoint( true, true );
		}
		ping.huds[ ping.huds.size ] = h;
	}
	self.mw2coop_pings[ self.mw2coop_pings.size ] = ping;
	self.mw2coop_ping_last = undefined;
	if ( kind == "place" )
		self.mw2coop_ping_last = ping;
	ping thread life();
	if ( ping.ai )
		ping thread death_watch( ent );
	voice = self say( kind, title, pos );
	ping.voice = voice;
	// the log (the mod's state line): who, what, where, when, the voice line; the QA reads it on both PCs
	n = 1;
	if ( isdefined( level.player2 ) && self == level.player2 )
		n = 2;
	if ( !isdefined( level.mw2coop_ping_made ) )
		level.mw2coop_ping_made = 0;
	level.mw2coop_ping_made++;
	what = kind;
	if ( isdefined( ent ) )
		what = kind + " (entity " + ent GetEntityNumber() + ")";
	if ( isdefined( title ) )
		what = what + " " + title;
	log_line( "ping " + level.mw2coop_ping_made + ": player " + n + " " + what + " at " + int( pos[ 0 ] ) + " " + int( pos[ 1 ] ) + " " + int( pos[ 2 ] ) + ", level time " + GetTime() + ", voice " + voice );
}

life()
{
	self endon( "gone" );
	ms = 12000;
	k = self.kind;
	if ( k == "enemy" || k == "enemy_turret" )
		ms = 7000;
	else if ( k == "weapon" || k == "ammo" || k == "mine" )
		ms = 15000;
	else if ( k == "danger" || k == "enemy_vehicle" || k == "vehicle" || k == "attack" )
		ms = 10000;
	else if ( k == "grenade" )
		ms = 8000;
	end = GetTime() + ms;
	while ( GetTime() < end )
	{
		if ( isdefined( self.ent ) || self.group != "place" )
		{
			e = self.ent;
			if ( !isdefined( e ) )
				break;	// picked up, exploded, deleted
			if ( self.ai && !IsAlive( e ) )
				break;
			if ( k == "enemy_vehicle" || k == "vehicle" )
				if ( !is_vehicle( e ) )
					break;	// a wreck now (script_vehicle_corpse)
			if ( IsSubStr( k, "explosive" ) && isdefined( e.exploded ) )
				break;
			if ( k == "enemy_turret" )
			{
				owner = e GetTurretOwner();
				if ( !isdefined( owner ) || !is_enemy( owner ) )
					break;	// the gunner left or died
			}
		}
		wait 0.25;
	}
	self remove();
}

// a marked man's death ends his marker at once (the "death" notify every AI gets; QA pingtest 2026-11-23: IsAlive on the killed man's entity did not end it)
death_watch( ent )
{
	self endon( "gone" );
	ent waittill( "death" );
	self remove();
}

// (the notify "gone" comes last: remove() is called from life() and death_watch(), which end on it, and a thread that notifies its own endon ends there: the
// marker's icons stayed for good after its time was up, QA pingtest 2026-11-23)
remove()
{
	if ( !isdefined( self.huds ) )
		return;
	foreach ( h in self.huds )
		if ( isdefined( h ) )
			h Destroy();
	self.huds = undefined;
	if ( isdefined( self.anchor ) )
		self.anchor Delete();
	self.anchor = undefined;
	owner = self.owner;
	if ( isdefined( owner ) && isdefined( owner.mw2coop_pings ) )
	{
		rest = [];
		foreach ( ping in owner.mw2coop_pings )
			if ( ping != self )
				rest[ rest.size ] = ping;
		owner.mw2coop_pings = rest;
		if ( isdefined( owner.mw2coop_ping_last ) && owner.mw2coop_ping_last == self )
			owner.mw2coop_ping_last = undefined;
	}
	self notify( "gone" );
}

// ---- the wheel

// the wheel's sectors, clockwise from the top (src/ping.c WHEEL_SECTORS picks one of as many); "need ammo" and "on me" were added on 2026-11-23 (the night list)
wheel_count()
{
	return 10;
}

wheel_kind( i )
{
	switch ( i )
	{
		case 0: return "enemy";
		case 1: return "attack";
		case 2: return "go";
		case 3: return "wait";
		case 4: return "danger";
		case 5: return "fallback";
		case 6: return "watch";
		case 7: return "cover";
		case 8: return "needammo";
		default: return "come";
	}
}

wheel_text( i )
{
	switch ( i )
	{
		case 0: return &"MW2COOP_PING_W_ENEMY";
		case 1: return &"MW2COOP_PING_W_ATTACK";
		case 2: return &"MW2COOP_PING_W_GO";
		case 3: return &"MW2COOP_PING_W_WAIT";
		case 4: return &"MW2COOP_PING_W_DANGER";
		case 5: return &"MW2COOP_PING_W_FALLBACK";
		case 6: return &"MW2COOP_PING_W_WATCH";
		case 7: return &"MW2COOP_PING_W_COVER";
		case 8: return &"MW2COOP_PING_W_NEEDAMMO";
		default: return &"MW2COOP_PING_W_COME";
	}
}

wheel_elem( x, y, sort )
{
	h = NewClientHudElem( self );
	h.horzAlign = "center";
	h.vertAlign = "middle";
	h.alignX = "center";
	h.alignY = "middle";
	h.x = x;
	h.y = y;
	h.sort = sort;
	h.foreground = true;
	h.hidewheninmenu = true;
	h.alpha = 1;
	return h;
}

wheel_open()
{
	if ( isdefined( self.mw2coop_wheel ) )
		return;
	w = SpawnStruct();
	w.sel = -1;
	w.boxes = [];
	w.labels = [];
	step = 360 / wheel_count();
	for ( i = 0; i < wheel_count(); i++ )
	{
		x = sin( i * step ) * 200;	// (ten boxes of 116 x 24 round this ellipse do not touch; eight sat on 124 x 88)
		y = 0 - cos( i * step ) * 120;
		box = self wheel_elem( x, y, 900 );
		box SetShader( "white", 116, 24 );
		box.color = ( 0, 0, 0 );
		box.alpha = 0.55;
		label = self wheel_elem( x, y, 901 );
		label.font = "default";
		label.fontScale = 1.4;
		label SetText( wheel_text( i ) );
		w.boxes[ i ] = box;
		w.labels[ i ] = label;
	}
	// under the wheel: the bottom box ("fall back", y 120, 24 high) ends at 132; at 124 the hint lay on it (owner, 2026-11-26)
	w.hint = self wheel_elem( 0, 160, 901 );
	w.hint.font = "default";
	w.hint.fontScale = 1.4;
	w.hint.alpha = 0.8;
	w.hint SetText( &"MW2COOP_PING_W_HINT" );
	w.opened = GetTime();
	self.mw2coop_wheel = w;
	self thread wheel_watch( w );
	log_line( "ping wheel: player " + player_n( self ) + " open, level time " + GetTime() );
}

// a wheel whose release never came (the press dropped by a menu sends one; this is the last guard): gone after 15 s
wheel_watch( w )
{
	self endon( "death" );
	w endon( "closed" );
	wait 15;
	if ( isdefined( self.mw2coop_wheel ) && self.mw2coop_wheel == w )
		self wheel_close( -1 );
}

wheel_sel( n )
{
	w = self.mw2coop_wheel;
	if ( !isdefined( w ) )
		return;
	if ( w.sel >= 0 && w.sel < wheel_count() )
	{
		w.boxes[ w.sel ].color = ( 0, 0, 0 );
		w.boxes[ w.sel ].alpha = 0.55;
	}
	w.sel = n;
	if ( n >= 0 && n < wheel_count() )
	{
		c = color_of( wheel_kind( n ), self );
		w.boxes[ n ].color = ( c[ 0 ] * 0.7, c[ 1 ] * 0.7, c[ 2 ] * 0.7 );
		w.boxes[ n ].alpha = 0.9;
	}
}

wheel_close( n )
{
	w = self.mw2coop_wheel;
	if ( !isdefined( w ) )
		return;
	self.mw2coop_wheel = undefined;
	foreach ( h in w.boxes )
		h Destroy();
	foreach ( h in w.labels )
		h Destroy();
	w.hint Destroy();
	log_line( "ping wheel: player " + player_n( self ) + " closed on " + n + ", level time " + GetTime() );
	w notify( "closed" );	// (after the HUD is gone: wheel_watch() calls this and ends on the notify)
	if ( n < 0 || n >= wheel_count() || !self spam_ok() )
		return;
	self wheel_ping( wheel_kind( n ) );
}

// the wheel's choice where he looks: "enemy", "attack" and "danger" take what is under the cross hair as the press does (an enemy, a vehicle, a turret; a grenade,
// something that explodes), else the spot
wheel_ping( kind )
{
	// "need ammo": a marker over himself that follows him (the partner then sees "press Use to give ammo" on him: maps\_mw2coop_partner::ammo_offer)
	if ( kind == "needammo" )
	{
		self.mw2coop_ammo_asked = GetTime();
		self make( "needammo", self, self.origin, undefined );
		return;
	}
	// "on me": a marker where he stands, and the partner is brought beside him where the game lets (maps\_mw2coop_partner::come_to)
	if ( kind == "come" )
	{
		self make( "come", undefined, self.origin + ( 0, 0, 8 ), undefined );
		self thread maps\_mw2coop_partner::come_to();
		return;
	}
	eye = self GetEye();
	dir = AnglesToForward( self GetPlayerAngles() );
	trace = BulletTrace( eye, eye + dir * 15000, true, self );
	if ( kind == "enemy" || kind == "attack" || kind == "danger" )
	{
		t = self target_at( eye, dir, trace );
		if ( isdefined( t ) )
		{
			enemy = t.kind == "enemy" || t.kind == "enemy_vehicle" || t.kind == "enemy_turret";
			danger = t.kind == "grenade" || t.kind == "mine" || IsSubStr( t.kind, "explosive" );
			if ( kind == "attack" && enemy )
			{
				self make( "attack", t.ent, t.pos, undefined );
				return;
			}
			if ( ( kind == "enemy" && enemy ) || ( kind == "danger" && danger ) )
			{
				self make( t.kind, t.ent, t.pos, t.title );
				return;
			}
		}
	}
	self make_spot( kind, trace[ "position" ] );
}

// the log (the mod's state line, which the DLL reads every 250 ms): one line every 0.3 s from a queue, so that none is lost when two come in one frame (a wheel let
// go and its marker); the QA compares the lines of both PCs
log_line( text )
{
	if ( !isdefined( level.mw2coop_ping_lines ) )
	{
		level.mw2coop_ping_lines = [];
		level thread log_emit();
	}
	level.mw2coop_ping_lines[ level.mw2coop_ping_lines.size ] = text;
}

log_emit()
{
	for ( ;; )
	{
		while ( !level.mw2coop_ping_lines.size )
			wait 0.05;
		text = level.mw2coop_ping_lines[ 0 ];
		rest = [];
		for ( i = 1; i < level.mw2coop_ping_lines.size; i++ )
			rest[ rest.size ] = level.mw2coop_ping_lines[ i ];
		level.mw2coop_ping_lines = rest;
		maps\_mw2coop::state( text );
		wait 0.3;
	}
}

player_n( player )
{
	if ( isdefined( level.player2 ) && player == level.player2 )
		return 2;
	return 1;
}

// ---- the line on the screen and the voice
// The single player game has iPrintLn only as a function for everyone, not as a player's method (a player method made the script fail to compile, 2026-11-23): one
// line for both (network co-op: each PC shows it to its player). The strings are the mod's (src/localize.c), "&&1" the player's name, "&&2" a weapon's.
say( kind, title, pos )
{
	name = "Alpha";
	if ( player_n( self ) == 2 )
		name = "Bravo";
	switch ( kind )
	{
		case "enemy": iPrintLn( &"MW2COOP_PING_ENEMY", name ); break;
		case "enemy_vehicle": iPrintLn( &"MW2COOP_PING_ENEMY_VEHICLE", name ); break;
		case "vehicle": iPrintLn( &"MW2COOP_PING_VEHICLE", name ); break;
		case "enemy_turret": iPrintLn( &"MW2COOP_PING_ENEMY_TURRET", name ); break;
		case "turret": iPrintLn( &"MW2COOP_PING_TURRET", name ); break;
		case "grenade": iPrintLn( &"MW2COOP_PING_GRENADE", name ); break;
		case "mine": iPrintLn( &"MW2COOP_PING_MINE", name ); break;
		case "explosive":
		case "explosive_barrel":
		case "explosive_car": iPrintLn( &"MW2COOP_PING_EXPLOSIVE", name ); break;
		case "weapon":
			if ( !isdefined( title ) )
				title = "?";
			iPrintLn( &"MW2COOP_PING_WEAPON", name, title );
			break;
		case "ammo": iPrintLn( &"MW2COOP_PING_AMMO", name ); break;
		case "danger": iPrintLn( &"MW2COOP_PING_DANGER", name ); break;
		case "attack": iPrintLn( &"MW2COOP_PING_ATTACK", name ); break;
		case "go": iPrintLn( &"MW2COOP_PING_GO", name ); break;
		case "wait": iPrintLn( &"MW2COOP_PING_WAIT", name ); break;
		case "fallback": iPrintLn( &"MW2COOP_PING_FALLBACK", name ); break;
		case "watch": iPrintLn( &"MW2COOP_PING_WATCH", name ); break;
		case "cover": iPrintLn( &"MW2COOP_PING_COVER", name ); break;
		case "needammo": iPrintLn( &"MW2COOP_PING_NEEDAMMO", name ); break;
		case "come": iPrintLn( &"MW2COOP_PING_COME", name ); break;
		case "cancel": iPrintLn( &"MW2COOP_PING_CANCEL", name ); break;
		default: iPrintLn( &"MW2COOP_PING_PLACE", name ); break;
	}
	if ( kind == "cancel" )
		return "none";
	alias = self voice_line( kind, pos );
	if ( !isdefined( alias ) )
		alias = "SP_hit_alert";
	else
		self.mw2coop_voice_at = GetTime();
	// once on each machine: player 1's local sound on his, player 2's on his own in network co-op (on one machine, the split screen, player 1's is heard already;
	// both would be two different takes of the line at once)
	level.player PlayLocalSound( alias );
	if ( isdefined( level.player2 ) && level.player2 != level.player && !GetDvarInt( "mw2coop_native_split" ) )
		level.player2 PlayLocalSound( alias );
	return alias;
}

// The squad's battle chatter (animscripts\battlechatter), the lines the level's own soldiers say, in the voice of the players' side (the allies' own: Rangers "US",
// Task Force 141 "TF", SEALs "NS", SAS "UK"), player 1 with voice 1 and player 2 with voice 2 of it, so the two sound different. Only the
// lines the level loads play (SoundExists; S.S.D.D. and Boneyard load none of the players' side); a marker the game has no line for keeps the short sound:
//   an enemy, his vehicle or turret, the wheel's "enemy" -> "contact at your <n> o'clock" from the partner's view (the line the squad says to the player), else "contact,
//                                                        <n> o'clock", else (SAS) "enemy machine gun" for a turret, "enemy in the open", "contact"
//   a grenade                                            -> "grenade!" (what the squad shouts when one lands)
//   the wheel's "attacking"                              -> the order to suppress (what a squad leader says in a fight)
//   the wheel's "going here"                             -> the order to move in a fight (what a soldier says as he moves up)
// Not more than one line every 1.5 s from one player (the next marker in that time gets the short sound): no lines over each other.
voice_line( kind, pos )
{
	if ( isdefined( self.mw2coop_voice_at ) && GetTime() - self.mw2coop_voice_at < 1500 )
		return undefined;
	c = voice_country();
	if ( c == "" )
		return undefined;
	v = c + "_" + player_n( self ) + "_";
	lines = [];
	switch ( kind )
	{
		case "enemy":
		case "enemy_vehicle":
		case "enemy_turret":
			other = level.players[ 0 ];
			if ( other == self )
				other = level.players[ 1 ];
			if ( isdefined( pos ) && isdefined( other ) )
			{
				clock = animscripts\battlechatter::getDirectionFacingClock( other GetPlayerAngles(), other.origin, pos );
				lines[ lines.size ] = v + "callout_yourclock_" + clock;
				lines[ lines.size ] = v + "callout_contactclock_" + clock;
			}
			if ( kind == "enemy_turret" )
				lines[ lines.size ] = v + "threat_mg_generic";
			lines[ lines.size ] = v + "threat_infantry_exposed";
			lines[ lines.size ] = v + "exposed_open";
			lines[ lines.size ] = v + "threat_infantry_generic";
			break;
		case "grenade":
			lines[ lines.size ] = v + "inform_incoming_grenade";
			break;
		case "attack":
			lines[ lines.size ] = v + "order_action_suppress";
			break;
		case "go":
			lines[ lines.size ] = v + "order_move_combat";
			break;
		case "come":	// "on me" (the squad's "follow me")
			lines[ lines.size ] = v + "order_move_follow";
			lines[ lines.size ] = v + "order_move_noncombat";
			break;
		case "needammo":	// (the game's soldiers have no "need ammo": "reloading!")
			lines[ lines.size ] = v + "inform_reloading_generic";
			break;
	}
	foreach ( alias in lines )
		if ( SoundExists( alias ) )
			return alias;
	return undefined;
}

// the side whose voices the markers use: the one most of the allied soldiers speak with (their countryID, animscripts\battlechatter_ai), else the campaign's (as the
// game picks the player's own; Contingency says "american" and loads no Rangers' lines: the QA of 2026-11-23), else the first of the four the level loads for both
// voices; "" when none (decided at the first marker, when the level's soldiers are there)
voice_country()
{
	if ( isdefined( level.mw2coop_voice_country ) )
		return level.mw2coop_voice_country;
	ids = [];
	count = [];
	best = undefined;
	foreach ( ai in GetAIArray( "allies" ) )
	{
		if ( !isdefined( ai.countryID ) || !IsAlive( ai ) )
			continue;
		id = ai.countryID;
		if ( !isdefined( count[ id ] ) )
			count[ id ] = 0;
		count[ id ]++;
		if ( !isdefined( best ) || count[ id ] > count[ best ] )
			best = id;
	}
	if ( isdefined( best ) )
		ids[ ids.size ] = best;
	if ( isdefined( level.campaign ) && isdefined( anim.countryIDs ) && isdefined( anim.countryIDs[ level.campaign ] ) )
		ids[ ids.size ] = anim.countryIDs[ level.campaign ];
	ids[ ids.size ] = "TF";
	ids[ ids.size ] = "US";
	ids[ ids.size ] = "NS";
	ids[ ids.size ] = "UK";
	level.mw2coop_voice_country = "";
	foreach ( id in ids )
		if ( SoundExists( id + "_1_inform_incoming_grenade" ) && SoundExists( id + "_2_inform_incoming_grenade" ) )
		{
			level.mw2coop_voice_country = id;
			break;
		}
	campaign = "none";
	if ( isdefined( level.campaign ) )
		campaign = level.campaign;
	allies = "none";
	if ( isdefined( best ) )
		allies = best + " (" + count[ best ] + ")";
	what = level.mw2coop_voice_country;
	if ( what == "" )
		what = "none (the short sound)";
	log_line( "ping voices: " + what + " (allies " + allies + ", campaign " + campaign + ")" );
	return level.mw2coop_voice_country;
}
