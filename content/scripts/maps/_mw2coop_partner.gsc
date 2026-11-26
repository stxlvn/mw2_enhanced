// The partner's distance, and a mark while he is down (owner 2026-11-23, the night list: "an arrow to the partner at the screen's edge with the distance, when he
// is not seen; a mark of its own when he is down and needs reviving"), and the marker wheel's partner orders ("on me", "need ammo").
//
// The arrow itself is the game's: maps\_coop gives each player a waypoint of the other (friendlyIcon: the co-op player marker, pinned to the screen's edge and
// shown only while he is off the screen; the health icon while he is down), and while he is down the game writes "Partner down" with the bleed-out time in
// the middle. The distance in metres is drawn on that icon by the game's own waypoint text (src/markers.c, owner 2026-11-24: "put the distance on the
// markers that are there, rather than build extra things"); the edge lines this file drew before ("< 13 m", "13 m >", "13 m behind", "Down: 13 m") are gone.
#include common_scripts\utility;
#include maps\_utility;

partner_precache()
{
	PrecacheString( &"MW2COOP_AMMO_GIVE_HINT" );
	PrecacheString( &"MW2COOP_AMMO_GIVEN" );
	PrecacheString( &"MW2COOP_AMMO_NONE" );
	PrecacheString( &"MW2COOP_COME_NOT_NOW" );
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
		player thread ammo_offer();
		player thread icon_near();
	}
}

// The partner's marker goes while he is near (owner 2026-11-26: "closer than 5 metres, no distance and the marker disappears"): maps\_coop's friendlyIcon,
// with the distance src/markers.c draws on it, is hidden (alpha 0) on this player's screen while the eyes are under 5 m apart, and comes back past 5.3 m (no
// flicker at the border) if the game shows it (coop_show_constant_icon, as maps\_coop::player_setup_icon does). Set at the end of each frame: the icon's flash
// when the partner fires or is hit (FriendlyHudIcon_FlashIcon) builds it anew with alpha 1 within the frame. self: the player whose screen shows it.
icon_near()
{
	self endon( "death" );
	level endon( "mw2coop_level_end" );
	other = get_other_player( self );
	near = false;
	for ( ;; )
	{
		wait 0.05;
		waittillframeend;
		if ( !isdefined( self.friendlyIcon ) || !isdefined( other ) )
			continue;
		d = Distance( self GetEye(), other GetEye() );
		if ( d < 196.85 || ( near && d < 208.7 ) )	// 5 m, back past 5.3 m (inches)
		{
			self.friendlyIcon.alpha = 0;
			near = true;
		}
		else if ( near )
		{
			near = false;
			if ( ( !flag_exist( "coop_show_constant_icon" ) || flag( "coop_show_constant_icon" ) ) && !isdefined( self.noFriendlyHudIcon ) )
				self.friendlyIcon.alpha = 1;
		}
	}
}

// a line on this player's screen only, for 2.5 s (iPrintLnBold shows on both)
tell( text )
{
	self notify( "mw2coop_tell" );
	self endon( "mw2coop_tell" );
	if ( isdefined( self.mw2coop_tell_hud ) )
		self.mw2coop_tell_hud Destroy();
	h = NewClientHudElem( self );
	h.horzAlign = "center";
	h.vertAlign = "middle";
	h.alignX = "center";
	h.alignY = "middle";
	h.y = 60;
	h.font = "default";
	h.fontscale = 1.4;
	h.alpha = 1;
	h.hidewheninmenu = true;
	h SetText( text );
	self.mw2coop_tell_hud = h;
	wait 2.5;
	h FadeOverTime( 0.5 );
	h.alpha = 0;
	wait 0.5;
	h Destroy();
	self.mw2coop_tell_hud = undefined;
}

name_of( player )
{
	if ( isdefined( level.player2 ) && player == level.player2 )
		return "Bravo";
	return "Alpha";
}

is_down( player )
{
	return player ent_flag_exist( "coop_downed" ) && player ent_flag( "coop_downed" );
}

// ---- "on me" (the marker wheel, maps\_mw2coop_ping::wheel_ping): the partner is brought beside the caller (owner 2026-11-23: a call by hand outside the
// story's scenes; the automatic one only follows a partner five checkpoints behind, and a player stuck where the game cannot follow, as after the drop in
// Favela, stays stuck). Not while either is down, rides or is held by a scene (linked), not where the mod hides the partner's marker (a scene's flag
// coop_show_constant_icon), not where a level turns the leash off, and not for a partner who is already near (8 m).
come_allowed( other )
{
	if ( !isdefined( other ) || !IsAlive( other ) || !IsAlive( self ) )
		return false;
	if ( is_down( self ) || is_down( other ) || self IsLinked() || other IsLinked() )
		return false;
	if ( isdefined( level.mw2coop_no_leash ) && level.mw2coop_no_leash )
		return false;
	if ( flag_exist( "coop_show_constant_icon" ) && !flag( "coop_show_constant_icon" ) )
		return false;
	return Distance( self.origin, other.origin ) > 320;
}

come_to()
{
	other = get_other_player( self );
	if ( !self come_allowed( other ) )
	{
		if ( isdefined( other ) && Distance( self.origin, other.origin ) > 320 )
			self thread tell( &"MW2COOP_COME_NOT_NOW" );
		maps\_mw2coop_ping::log_line( "come: player " + maps\_mw2coop_ping::player_n( self ) + " called, not now" );
		return;
	}
	wait 0.5;	// (the line and the voice first)
	moved = other maps\_mw2coop::teleport_to_partner_when_safe( self, 4 );	// (beside him on his floor with a clear line, or nothing while he is not on the ground)
	maps\_mw2coop_ping::log_line( "come: player " + maps\_mw2coop_ping::player_n( self ) + " called the partner over: " + moved + ", level time " + GetTime() );
}

// ---- ammo for the partner (owner 2026-11-23): a player who asked for ammo on the wheel ("need ammo", the last 30 s), or whose weapon has less than a
// magazine left in reserve, can be given some: the partner looks at him close by and presses Use ("Press [Use] to give ammo" on him). The giver gives one
// magazine of his own weapon's reserve, the other gets one magazine of his own weapon. Not while either is down, rides, or holds no weapon.
ammo_needed()
{
	w = self GetCurrentWeapon();
	if ( !isdefined( w ) || w == "none" || is_down( self ) || self IsLinked() )
		return false;
	stock = self GetWeaponAmmoStock( w );
	if ( stock >= WeaponMaxAmmo( w ) )
		return false; // (a full reserve takes nothing: asked or not, no offer; QA 2026-11-24: a full partner's "need ammo" cost the giver a magazine for nothing)
	if ( isdefined( self.mw2coop_ammo_asked ) && GetTime() - self.mw2coop_ammo_asked < 30000 )
		return true;
	clip = WeaponClipSize( w );
	return clip > 0 && stock < clip;
}

ammo_offer()
{
	self endon( "death" );
	use = Spawn( "script_model", self.origin + ( 0, 0, 40 ) );
	use SetModel( "tag_origin" );
	use LinkTo( self );
	self.mw2coop_ammo_use = use; // (QA wheelnew)
	usable = false;
	use thread ammo_use_watch( self );
	for ( ;; )
	{
		wait 0.25;
		want = self ammo_needed();
		if ( want != usable )
		{
			usable = want;
			if ( want )
			{
				use MakeUsable();
				use SetCursorHint( "HINT_NOICON" );
				use SetHintString( &"MW2COOP_AMMO_GIVE_HINT" );
			}
			else
				use MakeUnusable();
			maps\_mw2coop_ping::log_line( "ammo: player " + maps\_mw2coop_ping::player_n( self ) + " can be given ammo: " + want + ", level time " + GetTime() );
		}
	}
}

// self: the use entity on a player who needs ammo; the partner's Use gives it
ammo_use_watch( taker )
{
	taker endon( "death" );
	for ( ;; )
	{
		self waittill( "trigger", giver );
		if ( !isdefined( giver ) || !IsPlayer( giver ) || giver == taker || is_down( giver ) || giver IsLinked() )
			continue;
		gw = giver GetCurrentWeapon();
		tw = taker GetCurrentWeapon();
		if ( !isdefined( gw ) || gw == "none" || !isdefined( tw ) || tw == "none" )
			continue;
		gclip = WeaponClipSize( gw );
		tclip = WeaponClipSize( tw );
		have = giver GetWeaponAmmoStock( gw );
		if ( gclip <= 0 || tclip <= 0 || have < gclip )
		{
			giver thread tell( &"MW2COOP_AMMO_NONE" );
			continue;
		}
		// the taker gets what fits in his reserve, up to a magazine, and the giver gives that share of one of his (rounded up)
		room = WeaponMaxAmmo( tw ) - taker GetWeaponAmmoStock( tw );
		if ( room <= 0 )
			continue; // (full: the offer goes away with ammo_needed)
		got = tclip;
		if ( got > room )
			got = room;
		cost = gclip;
		if ( got < tclip )
			cost = int( ( gclip * got + tclip - 1 ) / tclip );
		giver SetWeaponAmmoStock( gw, have - cost );
		taker SetWeaponAmmoStock( tw, taker GetWeaponAmmoStock( tw ) + got );
		gclip = cost;
		tclip = got;
		taker.mw2coop_ammo_asked = undefined;
		iPrintLn( &"MW2COOP_AMMO_GIVEN", name_of( giver ), name_of( taker ) );
		if ( SoundExists( "weap_pickup" ) )
		{
			giver PlayLocalSound( "weap_pickup" );
			if ( !GetDvarInt( "mw2coop_native_split" ) )	// (one PC in the split screen: once)
				taker PlayLocalSound( "weap_pickup" );
		}
		maps\_mw2coop_ping::log_line( "ammo: " + name_of( giver ) + " gave " + gclip + " of " + gw + " (" + have + " left before), " + name_of( taker ) + " got " + tclip + " of " + tw + " (" + taker GetWeaponAmmoStock( tw ) + " now), level time " + GetTime() );
		wait 1;
	}
}
