// mw2coop QA: scripted checks that run inside the game on both PCs (lockstep, so they must be
// deterministic). Chosen before launch with the dvar mw2coop_qa on BOTH machines:
//   intro    - player 2 joins player 1 at the range, same weapons, not player 1's rifle
//   restart  - reach a checkpoint, down both players, expect a restart at that checkpoint
//   end      - (launch with "start end") run the ending, expect both machines on the next mission
//   resume   - (qa_pair.sh resume @continue) both machines start at the saved checkpoint
//   god      - with god mode on, heavy damage downs neither player
//   ride     - (invasion) player 2 rides in player 1's vehicle and gets out with him
//   turret   - (roadkill, start "ride") hand the Humvee turret from player 1 to player 2
//   board    - (roadkill, start "convoy") first to board is the gunner, the other rides, swaps
//   rideend  - (roadkill, start "ride_later") both get off at the end of the ride and the crash
//   rkintro  - (roadkill, default start) both players in Shepherd's scene, together afterwards
//   leash    - player 2 far away comes back to player 1
//   snow     - (cliffhanger, start "snowmobile") own snowmobiles; the ending waits for player 2
//   snowpick - (cliffhanger, start "icepick") player 2 takes player 1's snowmobile first
//   climb    - (cliffhanger, default start) both players climb the ice wall on their own lanes,
//              make the big jump, Soap catches both, both climb to the top (bot input)
//   spawn    - (any map/start) where player 2 is during the first 12 s
//   partnernear - (any map with room behind player 1) player 2 behind player 1 at 8, 3, 5.1, 6 and 4.5 m: his marker on player 1's view at 8 and 6 m only (hidden under 5 m, back past 5.3)
//   overlap  - (any map; Cliffhanger runs the climb bot) how close the players come (eyes, origins) and frames ov-N of both views when the eyes are under 30 units
//   takedown - (favela, default start) both ride the opening car (player 2 on the back seat),
//              both duck before Makarov's shots, both get out next to each other
//   takedown2 - same, but player 2 does not duck: Makarov hits him, not player 1
//   heliend - (dcburning, start "heliride2", native split) the Black Hawk is hit (flag player_heli_22): frames he-N of both players every 0.5 s for 8 s
//   crashdrift - (dcburning start "crash" / dcemp, pair) no input after the crash site fade-up: how far does each player drift in 24 s
//   empinput - (dcemp, default start, pair) no input from the crash site to the street: origin, link and the movement input of both players every second
//   crashwalk - (dcemp, default start, pair) real Forward/Back presses at the crash site once the scene lets the players move: can each player go back
//   oilrigambush - (oilrig, start "heli", pair) hostages alive and the ambush flags every 2 s for 90 s
//   framesevery - (any level and start) a frame of both players (a composed one in split screen) every second for 40 s: mw2coop\shots\fe-<n>.bmp, with both players' positions in the report
//   failspin - (split screen, any level) 12 s in the mission is failed (maps\_mw2coop::mission_failed); after the restart a frame every second for 20 s with player 2's yaw: does his half turn by itself (testers: after a failed mission his camera spins)?
//   weapnexttest - (pair, any level) the script asks for player 2's weapon key (dvar mw2coop_p2_weapnext): the client's DLL must press it, player 2's current weapon changes (issue 13)
//   hop - (any level, split screen or pair) 15 s after the level starts the mission is completed (the next mission is started the way the game does it); the scenario goes on in the next level: a chain of missions without a lobby (memory and crashes across loads)
//   falsefall - (any level, split screen or pair) a MOD_FALLING hit with no fall behind it is undone (Vlad's Oil Rig log), a real 300 unit fall still hurts
//   downaudio - (any level, split screen or pair) player 2 is downed for 20 s, then revived: the near-death sound filter (EQ/reverb flag) of each player every second.
//              Run with MW2COOP_MID "!trace <rva>" for seteqbands/setreverb/deactivateeq/deactivatereverb/playlocalsound to see who calls them and how often (Vlad: sound stutters after a partner is downed)
//   failspinkill - failspin, but both players really die (the death view) instead of the scripted fail; frames fk-N while they die
//   failspindown - player 2 is downed (shellshock, prone), 5 s later the mission fails; a frame every 0.5 s from each machine: fd-N (does the downed view turn during the fail?)
//   killp2 - player 2 only gets the engine's kill (100000 MOD_SUICIDE, what Vlad's log shows right before his downed camera tumbled); frames kp-N every second for 25 s: does his view tumble?
//   cookfrag - (native split, any level) player 2 holds his frag button (his pad through mw2coop_qa_pad2) for 8 s: the grenade cooks too long (Vlad's log: press frag, 4.1 s later 100000 MOD_SUICIDE, then his downed camera tumbled); frames ck-N every second for 30 s
//   downnone - player 2 has NO weapon in his hands (Vlad's log: w none) and goes down (stance prone); frames dn-N every second for 30 s: does his downed view tumble without a viewmodel?
//   hitdown - player 2 goes down, then player 1 hits him 10 times a second for 12 s (Vlad's clip: player 1 held his attack on the downed Bravo for 18 s and his camera tumbled); frames hd-N every 0.5 s
//   oilrigladder - (oilrig, start "heli") where the ambush scaffolding's nodes and trigger are and where both players stand: the first look for the ladder (owner: the ladder does not work for player 2)
//   oilrigclimb - (oilrig, start "heli") each player in turn stands at the foot of the ladder (1629 175 -1008, facing -y) and holds Forward for 8 s on his pad: how high does he get? (owner: the ladder does not work for player 2); frames lc1-N / lc2-N
//   oilrigclimbp - the same in a network pair (qa_pair.sh oilrigclimbp heli 60 oilrig): each player's own machine presses Forward (press()), not a pad dvar
//   oilrigclimbb - the split screen climb with the OTHER player standing at the top of the ladder (1629 118 -922, right at the exit), where the stock ambush has the first player wait: does the climber get stuck under / at him?
//   oilrigwhere - (oilrig, any start) where both players stand and what they see 8 s after the start (frame ow-N): which part of the rig a start point is
//   issshots - (dcemp, default start, pair) frames of both machines every 3 s through the ISS scene and the EMP: the two floating cameras must not show the other player's body or name (owner, 2026-11-06)
//   laptop2pair - (contingency, start "base", pair) the client (player 2) raises the Predator laptop (script switch) and puts it away with his stance key: his weapon after each
//   javpair - (pair, dcburning start "crows_nest_armor") player 2 is given the Javelin and switches to it: frames of both machines before, 3 s and 10 s after (owner: the client's textures vanish when he picks up the Javelin)
//   laptop2 - (contingency, start "ridge", native split with "p1pad=0;p0pad=1") player 2 gets the Predator laptop with the d-pad slot and the laptop thread; he raises it (d-pad right), then puts it away (Y): his weapon after each
//   allyrestore - (any level with a start point) the allies' places are snapshotted, one ally's recorded place is moved 1500 units away, the mission is failed: after the restart that ally must be at the recorded place (maps/_mw2coop restore_ally_positions)
//   squadgather - (any level, with a start point) how far the allies are from the nearest player 3 s and 14 s into the level: the squad is brought to player 1 after a restart at a checkpoint
//   airspeed - (airport, native split) both walk forward on their pads for 6 s after the elevator: the distance each covers
//   heldweapons - (any level) the weapon each player holds in his hands, every 2 s for 40 s (player 2 must not stand with empty hands while player 1 holds one)
//   gulagturn - (gulag, start "intro") player 2's view in the helicopter is turned to player 1 (what does his body look like from the next seat?); the angles of both and
//              of the helicopter are reported; frames come from MW2COOP_QA_CMDS shots
//   oilrigdoors - (oilrig, start "rig") the lower hostage room has two doors (breach group 100): player 1 stands at one door, player 2 at the other, player 1
//              presses Use (oilrigdoors2: player 2 presses; oilrigdoors3: both at the SAME door, the stock passive breacher). Both must breach, nobody dies, no hostage is killed
//   oilrigwait - (oilrig, start "rig") the tester's fail: player 1 waits at the lower hostage room's door for his partner with the level's stealth monitor on;
//              the mission must not fail (it did, 2 s after he reached the door: the tester's report); the level's alarms are reported as they come
//   takedown3 - player 2 ducks with REAL input: a network client presses his stance key (press(), mw2coop_bind +stance); takedown3s: split screen, his pad's B
//              through the dvar mw2coop_qa_pad2. Player 1 ducks through the notify. Makarov must not hit player 2 (Vlad's report: he could not duck)
//   crashsite - (dcburning, start "crash") the same crash site at the end of dcburning
//   dcemp    - (dcemp, default start) both wake up in the crashed Black Hawk, both get a rifle,
//              both float at the ISS, both come back to the wreck and are let out
//   street   - (dcemp, start "street") the falling helicopters: the street is safe only once both
//              players have hidden (the bot moves them into the hiding place)
//   nrend    - (airport, start "escape") the ending at the van: Makarov shoots both players
//   nrend2   - same, player 1 at the van's door (the close ending), player 2 a little away
//   knife    - (ending, start "wakeup") the knife fight: both wake up, player 1 strikes Shepherd
//              and plays the scene, player 2 goes down and watches; knife2: player 2 strikes
//   oilrig   - (oilrig, default start) both ride an SDV underwater, both surface; player 1 does
//              the stealth kill, then both are pulled out of the water. oilrig2: player 2 kills
//   loadout  - (any map) after the level's loadout both players hold the same weapon
//   cover    - (airport, start "stairs") player 2 fires at Makarov's squad: the cover is blown,
//              Makarov shoots both players and the mission fails for both
//   lineoffire - (airport, start "intro", MW2COOP_GOD=0) player 2 walks into the squad's line of
//              fire in the lobby: he dies, not player 1, and the mission fails for both
//   bodyview - (any map) both players 220 units apart, frames of each other standing, player 2
//              stepping sideways and backing off (the body's whole figure in motion)
//   boat     - (af_chase, default start) the Endgame boat chase: player 1 boards and drives,
//              player 2 rides as the gunner beside Price; the bot drives after the enemy boat.
//              boat2: player 2 boards first and drives
//   boatcp   - (af_chase, default start) Endgame's checkpoints (issue #16): none with the level's
//              start; boarding, the cave exit, the middle of the lake, the rapids, the river below
//              them and the boarding scene ahead are taken in order (the boat is put at each start
//              point's place); a failure in the middle of the lake restarts there with both players
//              in the boat and the later ones are still taken; a failure at the end restarts there
//   cpwalk   - (a level with flags or places in maps\_mw2coop_checkpoints, any start) no
//              checkpoint with the level's start; in the order of their sections player 1 is put on
//              each flag's trigger (a flag no map trigger sets is set) or just past each place, and
//              its start point must become the checkpoint
//   buttons  - (any map) real input: each player's own machine presses Forward and Use (press()),
//              he walks forward and the server sees his Use button, not the other player's
//   pickups  - (trainer; start "", "frag" or "pit") each player takes his own weapons off the
//              tables (looks at them, presses Use) and walks up to the frag box (Forward)
//   bay      - (trainer; a range start: "", timed_ads, bullet_penetration, frag) both players
//              stand inside the sandbag bay and can move
//   invertq  - (trainer, start "") the hip fire part: both get the range's rifle, player 1 faces the targets and each target
//              that pops up is hit for him; the level goes on to the sights training without single player's question
//              whether to invert the look (it froze player 1 alone)
//   partnerline - (any map; MW2COOP_GOD=0) the partner's distance drawn on the game's marker of him (src/markers.c answers in the log,
//              "qa: markers <step>: PASS|FAIL"): frames with player 2 out of player 1's view, in it, down in it and down out of it
//   wheelnew - (any map; Invasion for the voices) the wheel's "on me" brings player 2 beside player 1, and after player 2's
//              "need ammo" player 1's Use (real input) gives him a magazine from his own reserve
//   introfreeze - (a map's default start with an intro title) under the title neither player
//              moves on a real Forward press; afterwards both walk
//   tour     - (any map, default start) every playable section of the level (its start points), one
//              after another through restarts: both up, not stuck, both can walk, player 1 runs to
//              the two nearest story triggers and player 2 follows; both frames saved quietly at
//              each stop (mw2coop\shots, src/shot.c)
//   barrettpad - (dcburning, start "barrett", native split screen with player 2 on a pad: mw2coop_qa_pad2) player 2 gets on
//              the Barrett and turns its scope with the right stick a little (a quarter of the way past the dead zone) for
//              1 s: unzoomed, then zoomed in fully (the left stick forward); the turn in degrees and in screens, frames
//   betrayal - (estate, start "ending"; MW2COOP_GOD=0, god mode swallows Shepherd's shot) Loose
//              Ends' ending: both players go through Ghost's drag, the LZ, Shepherd's shot (it hits
//              both), the body toss and the fire as Roach, at his place
//   aimassist - (native split screen, a map with enemies; [native] p1pad=<n>) player 2's pad aim
//              assist (src/native.c aim_assist), his pad pressed through the dvar mw2coop_qa_pad2:
//              an enemy 400 units ahead; aiming down the sight 6 degrees beside it turns onto it;
//              a steady turn across it slows over it; a strafe past it keeps nearer with assist
//   bodycam  - (native split screen) player 1 looks at player 2, who is turned four ways and then
//              by his pad: player 2's body must face where his camera looks; the composed picture
//              of each (both halves) is saved through the dvar mw2coop_qa_frame
//   glyphs   - (trainer, native split screen, [native] p1pad=<n>) each player gets a hint naming a
//              key ("[{+gostand}]"): player 2 on a pad sees the button's picture, player 1 the key;
//              the composed picture is saved as "glyphs" (src/padglyphs.c)
//   slidesync - (favela_escape, start "solorun_chopper") player 1 steps onto the rooftop slide while player 2 is still at the start: player 2
//              is brought next to him (maps\_mw2coop_favelaescape::slide_sync), frame "slide"
//   groundpick - (native split screen, player 2 on a pad: MW2COOP_PAD=0; any level where player 2 holds a weapon) player 2 picks a weapon off the
//              ground (Vlad 2026-11-21: "a weapon he picks up shows only after switching once or twice"): one of his weapons is taken from him and laid
//              in front of him, his pad holds X (Use) on it; it must be in his hands without a switch of his own (frames gp-before, gp-after)
//   deadpick - (native split screen with player 2 on a pad: MW2COOP_PAD=0, or a network pair: his client holds +activate; invasion) a weapon lying in the
//              level (one player 2 lacks; none after 8 s: an AI near him dies by damage and drops one): player 2 is put beside it, looks at it and holds
//              Use; it must be his weapon in hands (frames dp-before, dp-after; the hands logged frame by frame); player 1 takes it if player 2 could not
//   deadpick3 - deadpick and three more pickups of the weapon lying nearest (the one he just dropped among them): each must stay in his hands 2.5 s
//              later, and sync_weapons' empty-hands rule must not press his weapon key during one (a player's report, 2026-11-25: "the game switched
//              the weapons he picked up")
//   pingtest - (native split screen; invasion: enemies, cars, Humvees) the co-op markers (maps\_mw2coop_ping.gsc), the script's events given straight: player 1
//              marks the ground in front (place), again on it (gone), twice quickly (danger), opens the wheel (frame ping-wheel), picks "watching" and lets go
//              (a watch marker); then, as targets come in sight (90 s at most): player 2 marks an enemy (enemy, an anchor above him), the weapon he drops when
//              killed (weapon, its name), a car that explodes (explosive) and a vehicle; frames ping-*. With the console lines of the DLL's wheel (MW2COOP_MID:
//              mw2coop_ping_qa 0 move <dx> 0 alone, then +mw2coop_ping, a move, -mw2coop_ping) it logs player 1's view yaw: the lone move turns it, the move
//              in the open wheel must not ("pingtest: yaw ..."). The voices: "watching" has no line (the short sound), "going here" and "attacking" from the
//              wheel are player 1's lines of the campaign's side, the enemy marker player 2's "contact at your <n> o'clock"
//   racestart - (native split screen, so_snowrace1_cliffhanger / so_snowrace2_cliffhanger; started by maps\_mw2coop, not by this scenario list) both players
//              hold the throttle from the level's start: player 1 his forward key (the console's "mw2coop_qa_move 1 0" in MW2COOP_PRE), player 2 his left stick up
//              (the dvar mw2coop_qa_pad2, set here); neither snowmobile may move before the race's "GO" (the flag race_started: the race freezes both players'
//              controls until then), both must move after it (frames race-*)
//   hintkeys - (native split screen, player 1 on the keyboard, player 2 on a pad; any level) the game's hints that name a key the PC's text got wrong or has no
//              default key for, printed big one after another on both halves (frames hint-<n>): player 1 must see a key (no "unbound"; the snowmobile's drive
//              W), player 2 his pad's button (src/padglyphs.c, src/localize.c command_fixes); the log names each stand-in ("padglyphs: a hint's ...")
//   pingvoice - (native split screen, any level) which side's voices the markers use there and which of their lines the level loads; player 1 picks "going here"
//              and player 2 "enemy" on the wheel: each marker must carry the line the level has for it (or the short sound when it has none)
//   edgeyaw  - (cliffhanger, default start) player 1 turns to eight directions on the cave ledge; each time player 2 is brought to him
//              (maps\_mw2coop::teleport_to_partner): beside him, "on his spot" or held back at the edge; then whether player 2 fell
//   laserdup - (any level with allies, native split screen) the four allies nearest player 1 get their laser forced on for 20 s, frames
//              "laser-a/b"; run it with the console line "!watchx a5920 @400518" (64-bit: the laser draw and the laser list's count, by
//              client context): each context should draw only its own lasers (the engine audit's row E)
// Results go to the dvar mw2coop_qa_log, which the DLL writes to mw2coop.log on each PC.
#include common_scripts\utility;
#include maps\_utility;

main()
{
	// a scenario that ends the mission marks itself done, so the next map does not rerun it
	if ( GetDvar( "mw2coop_qa_phase" ) == "done" )
		return;
	level.mw2coop_qa_n = 0;
	maps\_mw2coop::state( "qa scenario " + GetDvar( "mw2coop_qa" ) + " started" );
	wait 1.5;
	maps\_mw2coop::state( "qa running" );	// let start points, regroup and weapon sync run
	switch ( GetDvar( "mw2coop_qa" ) )
	{
		case "firecheck":
			firecheck();
			break;
		case "shadowcheck":
			shadowcheck();
			break;
		case "frags":
			frags();
			break;
		case "hintown":
			hintown();
			break;
		case "p2cmds":
			p2cmds();
			break;
		case "p2fire":
			p2fire();
			break;
		case "padlost":
			padlost();
			break;
		case "issplit":
			report( "IsSplitScreen " + IsSplitScreen() + ", is_coop " + is_coop() + ", is_coop_online " + is_coop_online() );
			break;
		case "snowturn":
			snowturn();
			break;
		case "ownswap":
			ownswap();
			break;
		case "laserpad":
			laserpad();
			break;
		case "predsteer":
			predsteer();
			break;
		case "fxplay":
			fxplay();
			break;
		case "fxhalf":
			fxhalf();
			break;
		case "hidep2":
			wait 5;
			level.player2 Hide();
			angles_log();
			break;
		case "hidep1":
			wait 5;
			level.player Hide();
			angles_log();
			break;
		case "angles":
			angles_log();
			break;
		case "walk50":
			walk50();
			break;
		case "p1far":
			p1far();
			break;
		case "p1front":
			p1front();
			break;
		case "p2away":
			p2away();
			break;
		case "p2mirror":
			p2mirror();
			break;
		case "tele":
			thread angles_log();
			tele();
			break;
		case "airreal":
			thread angles_log();
			airreal();
			break;
		case "p1move":
			p1move();
			break;
		case "p2ghost":
			p2ghost();
			break;
		case "sunlook":
			sunlook();
			break;
		case "airsee":
			airsee();
			break;
		case "aircmp":
			aircmp();
			break;
		case "airjump":
			thread angles_log();
			airjump();
			break;
		case "ac130fire":
			ac130fire();
			break;
		case "foleylook":
			foleylook();
			break;
		case "airwalk":
			thread angles_log();
			airwalk();
			break;
		case "p2walk":
			p2walk();
			break;
		case "walkpair":
			walkpair();
			break;
		case "veil":
			veil();
			break;
		case "screrr":
			screrr();
			break;
		case "face":
			face();
			break;
		case "aimhead":
			aimhead();
			break;
		case "pitchramp":
			pitchramp();
			break;
		case "gunface":
			gunface();
			break;
		case "hudtest":
			hudtest();
			break;
		case "weapsw":
			weapsw();
			break;
		case "intro":
			intro();
			break;
		case "padwalk":
			padwalk();
			break;
		case "failwalk":
			failwalk();
			break;
		case "restart":
			restart();
			break;
		case "end":
			ending();
			break;
		case "resume":
			resume();
			break;
		case "god":
			god();
			break;
		case "downs":
			downs();
			break;
		case "dmg":
			dmg();
			break;
		case "sceneskill":
			sceneskill();
			break;
		case "skill":
			skill();
			break;
		case "flares":
			flares();
			break;
		case "whbreach":
			whbreach();
			break;
		case "breachstuck":
			breachstuck();
			break;
		case "doomedcp":
			doomedcp();
			break;
		case "weapdrop":
			weapdrop();
			break;
		case "cpweapons":
			cpweapons();
			break;
		case "cpflags":
			cpflags();
			break;
		case "nativesave":
			nativesave();
			break;
		case "snapsave":
			snapsave();
			break;
		case "swaphint":
			swaphint();
			break;
		case "pilotfov":
			pilotfov();
			break;
		case "pvp":
			pvp();
			break;
		case "cpcheck":
			cpcheck();
			break;
		case "cprestart":
			cprestart();
			break;
		case "ride":
			ride();
			break;
		case "seatview":
			seatview();
			break;
		case "turret":
			turret();
			break;
		case "board":
			board();
			break;
		case "rideend":
			rideend();
			break;
		case "rkintro":
			rkintro();
			break;
		case "leash":
			leash();
			break;
		case "snow":
			snow();
			break;
		case "snowview":
			snowview();
			break;
		case "snowpick":
			snowpick();
			break;
		case "spawn":
			spawn_audit();
			break;
		case "takedown":
		case "takedown2":
			takedown();
			break;
		case "takedown3":
		case "takedown3s":
			takedown3();
			break;
		case "oilrig":
		case "oilrig2":
			oilrig();
			break;
		case "heliend":
			heliend();
			break;
		case "crashdrift":
			crashdrift();
			break;
		case "empinput":
			empinput();
			break;
		case "crashwalk":
			crashwalk();
			break;
		case "oilrigambush":
			oilrigambush();
			break;
		case "framesevery":
			framesevery();
			break;
		case "failspin":
		case "failspinkill":
			failspin();
			break;
		case "failspindown":
			failspindown();
			break;
		case "killp2":
			killp2();
			break;
		case "cookfrag":
			cookfrag();
			break;
		case "downnone":
			downnone();
			break;
		case "hitdown":
			hitdown();
			break;
		case "oilrigladder":
			oilrigladder();
			break;
		case "oilrigwhere":
			oilrigwhere();
			break;
		case "laptop2":
			laptop2();
			break;
		case "javpair":
			javpair();
			break;
		case "laptop2pair":
			laptop2pair();
			break;
		case "issshots":
			issshots();
			break;
		case "oilrigclimb":
		case "oilrigclimbp":
		case "oilrigclimbb":
		case "oilrigclimbbp":
			oilrigclimb();
			break;
		case "weapnexttest":
			weapnexttest();
			break;
		case "hop":
			hop();
			break;
		case "falsefall":
			falsefall();
			break;
		case "downaudio":
			downaudio();
			break;
		case "allyrestore":
			allyrestore();
			break;
		case "squadgather":
			squadgather();
			break;
		case "airspeed":
			airspeed();
			break;
		case "stealthp2":
			stealthp2();
			break;
		case "heldweapons":
			heldweapons();
			break;
		case "gulagturn":
			gulagturn();
			break;
		case "oilrigwait":
			oilrigwait();
			break;
		case "oilrigdoors":
		case "oilrigdoors2":
		case "oilrigdoors3":
			oilrigdoors();
			break;
		case "dcemp":
		case "crashsite":
			dcemp();
			break;
		case "street":
			street();
			break;
		case "nrend":
		case "nrend2":
			nrend();
			break;
		case "knife":
		case "knife2":
			knife();
			break;
		case "knifepose":
			knifepose();
			break;
		case "knifevar":
			knifevar();
			break;
		case "knifeaxis":
			knifeaxis();
			break;
		case "knifehand":
			knifehand();
			break;
		case "knifelook":
			knifelook();
			break;
		case "knifeframe":
			knifeframe();
			break;
		case "body":
			body();
			break;
		case "cover":
			cover();
			break;
		case "loadout":
			loadout();
			break;
		case "boat":
		case "boat2":
			boat();
			break;
		case "boatcp":
			boatcp();
			break;
		case "cpwalk":
			cpwalk();
			break;
		case "lineoffire":
			lineoffire();
			break;
		case "bodyview":
			bodyview();
			break;
		case "look":
			look();
			break;
		case "targets":
			targets();
			break;
		case "pickups":
			pickups();
			break;
		case "buttons":
			buttons();
			break;
		case "bay":
			bay();
			break;
		case "invertq":
			invertq();
			break;
		case "partnernear":
			partnernear();
			break;
		case "partnerline":
			partnerline();
			break;
		case "barrettpad":
			barrettpad();
			break;
		case "wheelnew":
			wheelnew();
			break;
		case "introfreeze":
			introfreeze();
			break;
		case "course":
			course();
			break;
		case "course2":
			course2();
			break;
		case "betrayal":
			betrayal();
			break;
		case "tour":
			tour();
			break;
		case "cptour":
			cptour();
			break;
		case "bbcheck":
			bbcheck();
			break;
		case "rappelview":
			rappelview();
			break;
		case "rappel":
			rappel();
			break;
		case "chopperjump":
			chopperjump();
			break;
		case "slidesync":
			slidesync();
			break;
		case "laserdup":
			laserdup();
			break;
		case "edgeyaw":
			edgeyaw();
			break;
		case "pingtest":
			pingtest();
			break;
		case "pingvoice":
			pingvoice();
			break;
		case "hintkeys":
			hintkeys();
			break;
		case "groundpick":
			groundpick();
			break;
		case "deadpick3":	// deadpick and three more pickups (the empty-hands rule must not switch his weapon in the middle of one)
			level.mw2coop_qa_repeat = true;
		case "deadpick":
			deadpick();
			break;
		case "scenewatch":
			scenewatch();
			break;
		case "gulagevac":
			gulagevac();
			break;
		case "oilbird":
			oilbird();
			break;
		case "invconvoy":
			invconvoy();
			break;
		case "bigjump":
			bigjump();
			break;
		case "climbview":
			if ( isdefined( level.mw2coop_qa_climbview ) )
				[[ level.mw2coop_qa_climbview ]]();
			else
				report( "climbview: not on Cliffhanger" );
			break;
		case "climblb":
			if ( isdefined( level.mw2coop_qa_climblb ) )
				[[ level.mw2coop_qa_climblb ]]();
			else
				report( "climblb: not on Cliffhanger" );
			break;
		case "overlap":	// how close the players come (eyes, bodies) and frames of both views then; Cliffhanger: the climb bot, else 30 s from the start
			level thread overlap_watch();
			if ( isdefined( level.mw2coop_qa_climb ) )
				[[ level.mw2coop_qa_climb ]]();
			else
			{
				overlap_look();
				wait 20;
				report( "done" );
			}
			break;
		case "climb":
			// in maps\_mw2coop_climb_qa, loaded on Cliffhanger only (it names maps\_climb, which
			// other maps' zones do not have)
			if ( isdefined( level.mw2coop_qa_climb ) )
				[[ level.mw2coop_qa_climb ]]();
			else
				report( "climb: not on Cliffhanger" );
			break;
		case "aimassist":
			aimassist();
			break;
		case "bodycam":
			bodycam();
			break;
		case "glyphs":
			glyphs();
			break;
		case "armed":
			armed();
			break;
		case "hudlayout":
			hudlayout();
			break;
		case "clock":
			clock();
			break;
		case "speed":
			speed();
			break;
		default:
			// the night audit's scenarios (2026-11-17): maps\_mw2coop_qa_bind
			if ( !maps\_mw2coop_qa_bind::dispatch( GetDvar( "mw2coop_qa" ) ) )
				report( "unknown scenario " + GetDvar( "mw2coop_qa" ) );
	}
}

report( text )
{
	level.mw2coop_qa_n++;
	SetDvar( "mw2coop_qa_log", level.mw2coop_qa_n + ": " + text );
	wait 0.3;	// the DLL polls every 250 ms
}

check( ok, text )
{
	if ( ok )
		report( "PASS " + text );
	else
		report( "FAIL " + text );
}

weapons_text( player )
{
	text = "";
	foreach ( weapon in player GetWeaponsListAll() )
		text += weapon + " ";
	return text;
}

common_checks()
{
	check( level.players.size == 2 && isdefined( level.player2 ), "two players (" + level.players.size + ")" );
	check( !is_specialop(), "campaign mode, not Spec Ops" );
	d = Distance( level.player.origin, level.player2.origin );
	check( d < 160, "player 2 next to player 1 (" + int( d ) + " units)" );
	report( "start point " + level.start_point + ", p1 weapons: " + weapons_text( level.player ) + "| p2 weapons: " + weapons_text( level.player2 ) );
}

// angles: (native split screen test) once a second: where each player is, where he looks (view angles) and which way the other one lies
// walk50: (native split screen test, "as on ice") player 2's server position and speed every 50 ms with the game time, to set against where the other context
// draws his body (native.c timelog) while a fake pad walks him forward and back
// p1front: (native split screen test) player 1 stands still 300 units in front of player 2 from 25 s on, for the whole run: does his body stay in player 2's half with time?
p1front()
{
	thread p1front_frames();
	level.mw2coop_no_follow = true;
	level.mw2coop_no_leash = true;
	wait 25;
	a = level.player;
	b = level.player2;
	yaw = b GetPlayerAngles()[ 1 ];
	a SetOrigin( b.origin + AnglesToForward( ( 0, yaw, 0 ) ) * 300 + ( 0, 0, 12 ) );
	a SetPlayerAngles( ( 0, yaw + 180, 0 ) );
	report( "p1front placed at t " + GetTime() + " p1 " + a.origin + " p2 " + b.origin );
}

// screrr: raises a script runtime error two functions deep (a field set on an integer), to see the call stack scrdebug logs ("at" and "from" lines with file::function)
screrr()
{
	wait 8;
	report( "screrr: calling" );
	screrr_a();
	report( "screrr: not reached" );
}

screrr_a()
{
	screrr_b();
}

screrr_b()
{
	u = 1;
	u.field = 2;	// a field of an integer: a runtime error
}

// veil: (native split screen test) both players face each other like in "armed"; the white haze around a body in the other player's half is checked against the game's
// own effects switch: a frame with fx_draw 1, a frame with fx_draw 0, a frame with fx_draw 1 again.
veil()
{
	if ( !flag_exist( "introscreen_complete" ) || !flag( "introscreen_complete" ) )
		level waittill_notify_or_timeout( "introscreen_complete", 30 );
	wait 2;
	p1 = level.player;
	p2 = level.player2;
	yaw = p1 GetPlayerAngles()[ 1 ] + GetDvarInt( "mw2coop_qa_yawoff" );
	ahead = p1.origin + AnglesToForward( ( 0, yaw, 0 ) ) * 250;
	p2 SetOrigin( PlayerPhysicsTrace( ahead + ( 0, 0, 48 ), ahead - ( 0, 0, 96 ) ) );
	for ( step = 0; step < 2; step++ )
	{
		if ( step == 1 )
			SetDvar( "fx_draw", "0" );
		else
			SetDvar( "fx_draw", "1" );
		for ( t = 0; t < 30; t++ )
		{
			p1 SetPlayerAngles( VectorToAngles( p2 GetEye() - ( 0, 0, 20 ) - p1 GetEye() ) );
			p2 SetPlayerAngles( VectorToAngles( p1 GetEye() - ( 0, 0, 20 ) - p2 GetEye() ) );
			wait 0.05;
		}
		// player 1 fires for 1.2 s; the frames are 0.4 s and 0.9 s into the burst
		press( p1, "attack", 1200 );
		for ( t = 0; t < 40; t++ )
		{
			p1 SetPlayerAngles( VectorToAngles( p2 GetEye() - ( 0, 0, 20 ) - p1 GetEye() ) );
			p2 SetPlayerAngles( VectorToAngles( p1 GetEye() - ( 0, 0, 20 ) - p2 GetEye() ) );
			if ( t == 5 )
				SetDvar( "mw2coop_qa_frame", "veil-" + step + "a" );
			if ( t == 14 )
				SetDvar( "mw2coop_qa_frame", "veil-" + step + "b" );
			wait 0.05;
		}
		wait 3;
	}
	SetDvar( "fx_draw", "1" );
	report( "done" );
}

// tele: (native split screen test) 20 s into the level player 2 is moved by a script (SetOrigin) 400 units ahead of player 1 on free ground, and again 15 s later 400 units to
// the other side: does his camera (native.c viewlog origin, context 1) follow his body (angles_log: the server position)?
tele()
{
	level.mw2coop_no_follow = true;
	level.mw2coop_no_leash = true;
	wait 20;
	a = level.player;
	b = level.player2;
	for ( i = 0; i < 2; i++ )
	{
		fwd = AnglesToForward( ( 0, a.angles[ 1 ] + i * 180, 0 ) );
		to = PlayerPhysicsTrace( a.origin + ( 0, 0, 16 ), a.origin + fwd * 400 + ( 0, 0, 16 ) );
		report( "tele: player 2 was at " + b.origin + ", is put at " + to );
		b SetOrigin( to );
		wait 15;
		report( "tele: player 2 now at " + b.origin );
	}
}

// airreal: (native split screen test, airport) the mod's own follow and leash stay ON; once the elevator ride is over player 2 walks forward on the pad dvar for 8 s, then
// stands: what do his camera (native.c viewlog origin, context 1) and his body (angles_log) do?
airreal()
{
	while ( !isdefined( level.mw2coop_elevator_node ) )
		wait 0.5;
	while ( isdefined( level.mw2coop_elevator_node ) )
		wait 0.5;
	report( "airreal: the ride is over, player 2 at " + level.player2.origin + ", player 1 at " + level.player.origin );
	wait 12;
	SetDvar( "mw2coop_qa_pad2", "0 32767 0 0 0 0 0 0" );
	wait 8;
	SetDvar( "mw2coop_qa_pad2", "0 0 0 0 0 0 0 0" );
	report( "airreal: player 2 stopped at " + level.player2.origin + ", player 1 at " + level.player.origin );
}

// p1move: (native split screen test) player 1 walks ahead 4 s from 20 s into the level (script steps along his view, stopped by walls), player 2 stands: does player 1's body in
// player 2's half move with him (native.c p1entlog)?
p1move()
{
	level.mw2coop_no_follow = true;
	level.mw2coop_no_leash = true;
	wait 20;
	for ( i = 0; i < 40; i++ )
	{
		f = AnglesToForward( ( 0, level.player GetPlayerAngles()[ 1 ], 0 ) );
		to = level.player.origin + f * 12;
		hit = PlayerPhysicsTrace( level.player.origin + ( 0, 0, 4 ), to + ( 0, 0, 4 ) );
		level.player SetOrigin( ( hit[ 0 ], hit[ 1 ], level.player.origin[ 2 ] ) );
		wait 0.1;
	}
	report( "p1move: player 1 at " + level.player.origin + ", player 2 at " + level.player2.origin );
}

// sunlook: (native split screen test, a sunny level, [native] sunlog=1) player 1 turns once around in 36 s from 20 s into the level (10 degrees per second), player 2 keeps his view:
// the sun state lines show whether player 1 looking into the sun moves player 2's state too
// Player 2 does not exist for the AI and the physics: not solid, ignored by every enemy and friend (jitter hunt: do the actors move unevenly in the split because of his body?)
p2ghost()
{
	level.mw2coop_no_follow = true;
	level.mw2coop_no_leash = true;
	wait 3;
	level.player2.ignoreme = true;
	level.player2.ignoreall = true;
	report( "p2ghost: player 2 is ignored by the AI" );
	if ( getdvar( "mw2coop_qa_arg" ) == "solid" )
	{
		wait 1;
		level.player2 SetContents( 0 );
		report( "p2ghost: player 2 has no contents" );
	}
}

sunlook()
{
	level.mw2coop_no_follow = true;
	level.mw2coop_no_leash = true;
	wait 20;
	p = level.player GetPlayerAngles();
	for ( i = 0; i < 72; i++ )
	{
		level.player SetPlayerAngles( ( p[ 0 ], p[ 1 ] + i * 5, 0 ) );
		if ( i % 4 == 0 )
			report( "sunlook: player 1 yaw " + level.player GetPlayerAngles()[ 1 ] + ", player 2 yaw " + level.player2 GetPlayerAngles()[ 1 ] );
		wait 0.5;
	}
}

// airsee: (native split screen test, airport) player 2 walks out as in airreal and the report gives both positions (a frame from the hall: does player 1 see player 2's body where he is?)
airsee()
{
	level.mw2coop_no_follow = true;
	level.mw2coop_no_leash = true;
	while ( !isdefined( level.mw2coop_elevator_node ) )
		wait 0.5;
	while ( isdefined( level.mw2coop_elevator_node ) )
		wait 0.5;
	wait 12;
	SetDvar( "mw2coop_qa_pad2", "0 32767 0 0 0 0 0 0" );
	wait 8;
	SetDvar( "mw2coop_qa_pad2", "0 0 0 0 0 0 0 0" );
	report( "airsee: player 2 stopped at " + level.player2.origin + ", player 1 at " + level.player.origin + " angles " + level.player GetPlayerAngles() + ", player 2 body model " + level.player2.model );
}

// aircmp: (native split screen test, airport) as airreal, then player 1 is put at player 2's spot with his angles: do both halves show the same picture (is the wall in
// player 2's half real geometry or a camera/render fault)?
aircmp()
{
	level.mw2coop_no_follow = true;
	level.mw2coop_no_leash = true;
	while ( !isdefined( level.mw2coop_elevator_node ) )
		wait 0.5;
	while ( isdefined( level.mw2coop_elevator_node ) )
		wait 0.5;
	wait 12;
	SetDvar( "mw2coop_qa_pad2", "0 32767 0 0 0 0 0 0" );
	wait 8;
	SetDvar( "mw2coop_qa_pad2", "0 0 0 0 0 0 0 0" );
	wait 1;
	a = level.player2 GetPlayerAngles();
	report( "aircmp: player 2 at " + level.player2.origin + " angles " + a + ", player 1 goes there" );
	eye = level.player2 GetEye();
	report( "aircmp: player 2 eye " + eye );
	for ( d = 0; d < 360; d += 45 )
	{
		dir = AnglesToForward( ( 0, d, 0 ) );
		t = BulletTrace( eye, eye + dir * 200, false, level.player2 );
		report( "aircmp: eye ray yaw " + d + " hits at " + int( Distance( eye, t[ "position" ] ) ) + " units, surface " + t[ "surfacetype" ] );
	}
	level.player SetOrigin( level.player2.origin );
	level.player SetPlayerAngles( a );
}

// airjump: (native split screen test, airport) after the elevator ride a script moves player 2 far across the hall (about 1000 units, then 1250), the way the mod's follow and
// checkpoint code does; does his camera (native.c viewlog origin, context 1) go with his body (angles_log: the server position)?
airjump()
{
	level.mw2coop_no_follow = true;
	level.mw2coop_no_leash = true;
	while ( !isdefined( level.mw2coop_elevator_node ) )
		wait 0.5;
	while ( isdefined( level.mw2coop_elevator_node ) )
		wait 0.5;
	wait 3;
	spots = [];
	spots[ 0 ] = ( 5551, 1940, 64.2 );
	spots[ 1 ] = ( 5290, 2168, 139.4 );
	spots[ 2 ] = ( 6540, 2043, 64.2 );
	for ( i = 0; i < spots.size; i++ )
	{
		report( "airjump: player 2 at " + level.player2.origin + " is put at " + spots[ i ] );
		level.player2 SetOrigin( spots[ i ] );
		wait 8;
		report( "airjump: player 2 is now at " + level.player2.origin );
	}
}

// airwalk: (native split screen test, airport) once the elevator ride is over and player 2 is let go, he walks forward for 4 s on the pad dvar: does his camera
// (native.c viewlog origin, context 1) go with his body (angles_log: the server position)?
airwalk()
{
	level.mw2coop_no_follow = true;
	level.mw2coop_no_leash = true;
	while ( !isdefined( level.mw2coop_elevator_node ) )
		wait 0.5;
	while ( isdefined( level.mw2coop_elevator_node ) )
		wait 0.5;
	report( "airwalk: player 2 is let go at " + level.player2.origin );
	wait 12;
	SetDvar( "mw2coop_qa_pad2", "0 32767 0 0 0 0 0 0" );
	wait 8;
	SetDvar( "mw2coop_qa_pad2", "0 0 0 0 0 0 0 0" );
	report( "airwalk: player 2 stopped at " + level.player2.origin );
}

// walkpair: (native split screen test, [native] p1pad=0 for player 2's pad dvar) from 15 s on the players take turns: one walks 2 s with a real Forward the way with the most
// free floor, the other looks at him. With [native] npclog=1;npcent=0 (or 1) the log has the walker's body as the other half draws it, frame by frame (jitter of the partner).
walkpair()
{
	level.mw2coop_no_follow = true;
	level.mw2coop_no_leash = true;
	wait 15;
	for ( r = 0; r < 4; r++ )
	{
		pair_walk( level.player, level.player2 );
		pair_walk( level.player2, level.player );
	}
	report( "walkpair done" );
}

pair_walk( walker, watcher )
{
	yaw = walker GetPlayerAngles()[ 1 ];
	room = 0;
	best = yaw;
	for ( turn = 0; turn < 360; turn += 30 )
	{
		from = walker.origin + ( 0, 0, 8 );
		free = Distance( from, PlayerPhysicsTrace( from, from + AnglesToForward( ( 0, yaw + turn, 0 ) ) * 300 ) );
		if ( free > room + 1 )
		{
			room = free;
			best = yaw + turn;
		}
	}
	walker SetPlayerAngles( ( 0, best, 0 ) );
	watcher SetPlayerAngles( ( 0, VectorToAngles( walker.origin - watcher.origin )[ 1 ], 0 ) );
	wait 0.2;
	start = walker.origin;
	if ( walker == level.player2 )
	{
		SetDvar( "mw2coop_qa_pad2", "0 32767 0 0 0 0 0 0" );
		wait 2;
		SetDvar( "mw2coop_qa_pad2", "0 0 0 0 0 0 0 0" );
	}
	else
	{
		press( walker, "forward", 2000 );
		wait 1.8;
	}
	report( "walkpair: " + player_name( walker ) + " walked " + int( Distance2D( walker.origin, start ) ) + " of " + int( room ) + " free" );
	wait 1;
}

// p2walk: (native split screen test) player 1 stands, player 2 walks forward / back / left / right on the pad dvar from 15 s on (1.5 s each, three rounds), the mod's follow and
// leash off; a composed frame every second with both players' server positions (p1front_frames): does player 2's body move in player 1's half when his camera does?
p2walk()
{
	thread p1front_frames();
	level.mw2coop_no_follow = true;
	level.mw2coop_no_leash = true;
	wait 15;
	for ( r = 0; r < 3; r++ )
	{
		SetDvar( "mw2coop_qa_pad2", "0 32767 0 0 0 0 0 0" );
		wait 1.5;
		SetDvar( "mw2coop_qa_pad2", "0 -32767 0 0 0 0 0 0" );
		wait 1.5;
		SetDvar( "mw2coop_qa_pad2", "-32767 0 0 0 0 0 0 0" );
		wait 1.5;
		SetDvar( "mw2coop_qa_pad2", "32767 0 0 0 0 0 0 0" );
		wait 1.5;
	}
	SetDvar( "mw2coop_qa_pad2", "0 0 0 0 0 0 0 0" );
}

// ac130fire: (native split screen test, so_ac130_co_hunted, set specialops 1) 20 s in, the gunner fires once with the game's own binding; a second later the native side saves a burst of
// consecutive frames ([native] burst11=-2): where does the shell land in his view, against the cross hair at the centre of his half?
ac130fire()
{
	level.mw2coop_no_follow = true;
	level.mw2coop_no_leash = true;
	wait 20;
	if ( !isdefined( level.ac130player ) )
	{
		report( "ac130fire: no gunner" );
		return;
	}
	report( "ac130fire: gunner is " + player_name( level.ac130player ) + ", firing" );
	press( level.ac130player, "attack", 200 );
	wait 0.8;
	SetDvar( "mw2coop_qa_burst", "1" );
	wait 8;
	report( "ac130fire: done" );
}

// foleylook: (native split screen test, Trainer) player 1's cross hair is put on the first friendly actor (Sgt. Foley) 25 s into the level and held for 12 s; player 2 looks elsewhere.
// The friendly's name shows under the cross hair of the player who aims at him: does it show in player 2's half too?
foleylook()
{
	level.mw2coop_no_follow = true;
	level.mw2coop_no_leash = true;
	wait 25;
	allies = GetAIArray( "allies" );
	if ( !allies.size )
	{
		report( "foleylook: no friendly actor" );
		return;
	}
	target = allies[ 0 ];
	for ( i = 0; i < 1500; i++ )
	{
		to = ( target.origin[ 0 ], target.origin[ 1 ], target.origin[ 2 ] + 60 ) - level.player GetEye();
		level.player SetPlayerAngles( VectorToAngles( to ) );
		wait 0.05;
	}
	report( "foleylook: done, target " + target.name );
}

// p2mirror (Vlad, 2026-11-20: in the split screen player 2 sees helicopters, targets and doors flicker and jerk, so_killspree_trainer): 20 s into the level player 2 is
// put next to player 1 with player 1's view angles, once, and both stand still; 2 s later the burst of frames is armed ([native] burst11=-2, burst11n): both halves show
// nearly the same scene, so a frame-to-frame difference of player 2's half against player 1's is the bug.
p2mirror()
{
	level.mw2coop_no_follow = true;
	level.mw2coop_no_leash = true;
	wait 20;
	a = level.player GetPlayerAngles();
	level.player2 SetOrigin( level.player.origin + AnglesToRight( ( 0, a[ 1 ], 0 ) ) * 32 );
	level.player2 SetPlayerAngles( a );
	report( "p2mirror: player 2 at " + level.player2.origin + " with player 1's angles " + a );
	wait 2;
	SetDvar( "mw2coop_qa_burst", "1" );
	report( "p2mirror: burst armed" );
}

// p2away: (native split screen test) player 2 is put about 350 units away from where he starts, on free ground (the mod's follow and leash are off), 25 s into the level:
// does a prop near his start (the rifle on the Trainer's table) keep its size in his half? A composed frame every second as in p1front.
p2away()
{
	thread p1front_frames();
	level.mw2coop_no_follow = true;
	level.mw2coop_no_leash = true;
	wait 25;
	b = level.player2;
	for ( a = 0; a < 360; a += 45 )
	{
		spot = b.origin + AnglesToForward( ( 0, a, 0 ) ) * 350;
		if ( !BulletTracePassed( b.origin + ( 0, 0, 48 ), spot + ( 0, 0, 48 ), false, b ) )
			continue;
		ground = PlayerPhysicsTrace( spot + ( 0, 0, 48 ), spot - ( 0, 0, 96 ) );
		if ( abs( ground[ 2 ] - b.origin[ 2 ] ) > 40 )
			continue;
		b SetOrigin( ground );
		report( "p2away moved to " + ground + " angle " + a );
		// then player 1 stands 250 units in front of him, facing him, so that each half shows the other player's body
		wait 2;
		yaw = b GetPlayerAngles()[ 1 ];
		level.player SetOrigin( b.origin + AnglesToForward( ( 0, yaw, 0 ) ) * 250 + ( 0, 0, 12 ) );
		level.player SetPlayerAngles( ( 0, yaw + 180, 0 ) );
		report( "p2away: player 1 placed " + level.player.origin + " p2 " + b.origin + " p2 server yaw " + yaw + " p1 yaw " + level.player GetPlayerAngles()[ 1 ] );
		if ( GetDvarInt( "mw2coop_qa_p1walk" ) )
		{
			// player 1 walks toward player 2 on the pad dvar for 6 s (his body runs in player 2's view: does its animation move as smoothly as the other actors'?)
			wait 3;
			if ( GetDvarInt( "mw2coop_qa_p1circle" ) )
				SetDvar( "mw2coop_qa_pad1", "0 32767 12000 0 0 0 0 0" ); // player 1 walks a circle on his pad dvar (forward, turning): the stepping of his leg animation is easier to see than in a straight walk
			else
				press( level.player, "forward", 5000 ); // the game's own binding, as padwalk does (the pad dvar did not move him)
			wait 0.7;
			SetDvar( "mw2coop_qa_burst", "1" ); // the native side saves a burst of consecutive frames when this is set ([native] burst11=-2)
			wait 4.5;
			SetDvar( "mw2coop_qa_pad1", "0 0 0 0 0 0 0 0" );
		}
		if ( GetDvarInt( "mw2coop_qa_p2walk" ) )
		{
			// player 2 walks on the pad dvar (forward, back, left, right, 1.2 s each, three rounds); the position lines come from p1front_frames
			wait 3;
			for ( r = 0; r < 3; r++ )
			{
				SetDvar( "mw2coop_qa_pad2", "0 32767 0 0 0 0 0 0" );
				wait 1.2;
				SetDvar( "mw2coop_qa_pad2", "0 -32767 0 0 0 0 0 0" );
				wait 1.2;
				SetDvar( "mw2coop_qa_pad2", "-32767 0 0 0 0 0 0 0" );
				wait 1;
				SetDvar( "mw2coop_qa_pad2", "32767 0 0 0 0 0 0 0" );
				wait 1;
			}
			SetDvar( "mw2coop_qa_pad2", "0 0 0 0 0 0 0 0" );
		}
		return;
	}
	report( "p2away found no free spot" );
}

p1front_frames()
{
	// a composed frame every 1 s from the start with a line in the log, to line the frames up with the native log's p0cnt/p0dump lines
	for ( i = 0; i < 70; i++ )
	{
		wait 1;
		SetDvar( "mw2coop_qa_frame", "pf-" + i );
		report( "pf " + i + " p1 " + level.player.origin + " p2 " + level.player2.origin );
	}
}

// p1far: (native split screen test) player 2 stands, player 1 is put in front of him at growing distances, 4 s each (the mod's follow and leash are off): is player 1's body
// drawn in player 2's half at every distance? Composed frames come from [native] seqat / seqstep.
p1far()
{
	level.mw2coop_no_follow = true;
	level.mw2coop_no_leash = true;
	wait 30;
	a = level.player;
	b = level.player2;
	yaw = b GetPlayerAngles()[ 1 ];
	fwd = AnglesToForward( ( 0, yaw, 0 ) );
	origin = b.origin;
	dists = [];
	dists[ 0 ] = 150;
	dists[ 1 ] = 500;
	dists[ 2 ] = 900;
	dists[ 3 ] = 1400;
	dists[ 4 ] = 1900;
	dists[ 5 ] = 2400;
	foreach ( d in dists )
	{
		a SetOrigin( origin + fwd * d + ( 0, 0, 12 ) );
		a SetPlayerAngles( ( 0, yaw, 0 ) );
		report( "p1far distance " + d + " t " + GetTime() + " p1 " + a.origin + " p2 " + b.origin );
		wait 4;
	}
	report( "p1far done" );
}

walk50()
{
	for ( ;; )
	{
		wait 0.05;
		b = level.player2;
		report( "walk50 t " + GetTime() + " p2 " + b.origin + " v " + b GetVelocity() );
	}
}

angles_log()
{
	for ( ;; )
	{
		wait 0.2;
		a = level.player;
		b = level.player2;
		to_b = VectorToAngles( b.origin - a.origin );
		to_a = VectorToAngles( a.origin - b.origin );
		report( "angles t " + GetTime() + " p1 at " + a.origin + " looks " + a GetPlayerAngles() + " | p2 at " + b.origin + " looks " + b GetPlayerAngles() + " | p1 to p2 yaw " + to_b[ 1 ] + ", p2 to p1 yaw " + to_a[ 1 ] );
	}
}

// face: (native split screen test) after the briefing, player 2 stands 100 units in front of player 1 and the two look at each other
face()
{
	wait 90;
	a = level.player;
	b = level.player2;
	fwd = AnglesToForward( ( 0, a.angles[ 1 ], 0 ) );
	b SetOrigin( a.origin + fwd * 100 );
	a SetPlayerAngles( ( 0, a.angles[ 1 ], 0 ) );
	b SetPlayerAngles( ( 0, a.angles[ 1 ] + 180, 0 ) );
	report( "face: p1 " + a.origin + " p2 " + b.origin );
}

// gunface: (native split screen test) face(), and both players hold a rifle
gunface()
{
	wait 100;
	for ( i = 0; i < 600; i++ )
	{
		SetSavedDvar( "cg_drawGun", 1 );
		level.player GiveWeapon( "m4_grunt" );
		level.player2 GiveWeapon( "m4_grunt" );
		level.player EnableWeapons();
		level.player2 EnableWeapons();
		level.player SwitchToWeapon( "m4_grunt" );
		level.player2 SwitchToWeapon( "m4_grunt" );
		wait 0.5;
	}
}

// aimhead: (split screen test) after the briefing, player 2 stands 200 units in front of player 1, both hold a rifle; player 1 aims at player 2's eyes and player 2 looks at player 1's eyes.
// Composed frames "aimhead-<n>" show where player 1's muzzle is in player 2's half (it must be on the line from player 2's eye, i.e. right at his crosshair, not lower).
aimhead()
{
	wait 45;
	level.mw2coop_no_follow = true;
	level.mw2coop_no_leash = true;
	a = level.player;
	b = level.player2;
	SetSavedDvar( "cg_drawGun", 1 );
	a GiveWeapon( "m4_grunt" );
	b GiveWeapon( "m4_grunt" );
	a SwitchToWeapon( "m4_grunt" );
	b SwitchToWeapon( "m4_grunt" );
	fwd = AnglesToForward( ( 0, a.angles[ 1 ], 0 ) );
	b SetOrigin( a.origin + fwd * 200 );
	for ( i = 0; i < 400; i++ )
	{
		ae = a.origin + ( 0, 0, 60 );
		be = b.origin + ( 0, 0, 60 );
		a SetPlayerAngles( VectorToAngles( be - ae ) );
		b SetPlayerAngles( VectorToAngles( ae - be ) );
		wait 0.05;
		if ( i == 100 || i == 200 || i == 300 )
		{
			SetDvar( "mw2coop_qa_frame", "aimhead-" + i );
			report( "aimhead " + i + ": p1 eye " + ae + " angles " + a GetPlayerAngles() + " | p2 eye " + be + " angles " + b GetPlayerAngles() );
		}
	}
}

// pitchramp: (split screen test) like aimhead, but player 1's pitch is stepped through -40 .. 40 degrees, 3 s each; the pitch is logged with the time, to be lined up with the native log's entdump lines
// of context 1's slot 1 (the body of player 1 as player 2's cgame sees it)
pitchramp()
{
	wait 45;
	level.mw2coop_no_follow = true;
	level.mw2coop_no_leash = true;
	a = level.player;
	b = level.player2;
	SetSavedDvar( "cg_drawGun", 1 );
	a GiveWeapon( "m4_grunt" );
	a SwitchToWeapon( "m4_grunt" );
	fwd = AnglesToForward( ( 0, a.angles[ 1 ], 0 ) );
	b SetOrigin( a.origin + fwd * 200 );
	b SetPlayerAngles( ( 0, a.angles[ 1 ] + 180, 0 ) );
	for ( p = -40; p <= 40; p += 20 )
	{
		for ( i = 0; i < 60; i++ )
		{
			a SetPlayerAngles( ( p, a.angles[ 1 ], 0 ) );
			wait 0.05;
		}
		report( "pitchramp t " + GetTime() + " p1 pitch set " + p + " now " + a GetPlayerAngles() );
		SetDvar( "mw2coop_qa_frame", "pitch" + ( p + 40 ) );
	}
}

// weapsw: (native split screen test) player 2's current weapon is logged every second: the pad's Y (fakepad.txt) must move it to the next of his list
weapsw()
{
	wait 100;
	level.player SwitchToWeapon( "usp_silencer" );
	for ( i = 0; i < 20; i++ )
	{
		report( "weapsw: p1 holds " + level.player GetCurrentWeapon() + ", p2 holds " + level.player2 GetCurrentWeapon() );
		wait 1;
	}
}

// hudtest: (native split screen test) a global hud element and one per player, to see which of them both halves draw
hudtest()
{
	wait 30;
	e = NewHudElem();
	e.x = 0;
	e.y = 60;
	e.alignX = "center";
	e.alignY = "middle";
	e.horzAlign = "center";
	e.vertAlign = "top";
	e.fontScale = 1.6;
	e.alpha = 1;
	e.sort = 100;
	e.foreground = true;
	e.hidewheninmenu = false;
	e.color = ( 1, 1, 0 );
	e SetText( "GLOBAL ELEM" );
	a = NewClientHudElem( level.player );
	a.x = -200;
	a.y = 100;
	a.alignX = "center";
	a.alignY = "middle";
	a.horzAlign = "center";
	a.vertAlign = "top";
	a.fontScale = 1.6;
	a.alpha = 1;
	a.sort = 100;
	a.foreground = true;
	a.hidewheninmenu = false;
	a.color = ( 1, 1, 0 );
	a SetText( "PLAYER ONE ELEM" );
	b = NewClientHudElem( level.player2 );
	b.x = 200;
	b.y = 100;
	b.alignX = "center";
	b.alignY = "middle";
	b.horzAlign = "center";
	b.vertAlign = "top";
	b.fontScale = 1.6;
	b.alpha = 1;
	b.sort = 100;
	b.foreground = true;
	b.hidewheninmenu = false;
	b.color = ( 1, 1, 0 );
	b SetText( "PLAYER TWO ELEM" );
	wait 600;
}

intro()
{
	common_checks();
	check( level.player2 GetWeaponsListAll().size == level.player GetWeaponsListAll().size, "same weapon count" );
	// a rifle player 1 gets stays his: each player takes his own off the range table (since
	// 2026-10-21; the real pickups are in the pickups scenario)
	level.player GiveWeapon( "m4_grunt" );
	level.player SwitchToWeapon( "m4_grunt" );
	wait 1;
	check( !level.player2 HasWeapon( "m4_grunt" ), "player 2 does not get player 1's rifle" );
	report( "done" );
}

restart()
{
	phase = GetDvar( "mw2coop_qa_phase" );
	if ( phase == "" )
	{
		common_checks();
		flag_set( "firing_range_hip_and_ads_done" );
		wait 1;
		check( GetDvar( "start" ) == "timed_ads", "checkpoint timed_ads reached (start dvar '" + GetDvar( "start" ) + "')" );
		SetDvar( "mw2coop_qa_phase", "after_restart" );
		report( "downing player 2" );
		level.player2 DoDamage( level.player2.health + 500, level.player2.origin );
		wait 1;
		check( level.player2 ent_flag( "coop_downed" ), "player 2 is down, not dead" );
		wait level.player_downed_death_buffer_time + 1;
		report( "downing player 1" );
		level.player DoDamage( level.player.health + 500, level.player.origin );
		wait 1;
		check( flag( "coop_alldowned" ), "both down -> all downed" );
		report( "waiting for the restart" );
		return;
	}
	if ( phase == "after_restart" )
	{
		SetDvar( "mw2coop_qa_phase", "" );
		check( level.start_point == "timed_ads", "restarted at checkpoint timed_ads (" + level.start_point + ")" );
		common_checks();
		check( !flag( "coop_alldowned" ), "players are up again" );
		report( "done" );
	}
}

ending()
{
	if ( GetDvar( "mw2coop_qa_phase" ) == "after_end" )
	{
		SetDvar( "mw2coop_qa_phase", "" );
		check( level.script == "roadkill", "next mission loaded (" + level.script + ")" );
		common_checks();
		report( "done" );
		return;
	}
	common_checks();
	SetDvar( "mw2coop_qa_phase", "after_end" );
	flag_set( "player_exiting_course_00" );
	wait 1;
	flag_set( "player_exiting_course" );
	wait 1;
	flag_set( "player_exiting_course_02" );
	report( "ending running" );
	wait 15;
	report( "FAIL still on " + level.script + " 15 s after the ending" );
}

// Continued session (qa_pair.sh resume @continue): both machines start at the saved checkpoint.
resume()
{
	common_checks();
	check( isdefined( level.start_point ) && level.start_point != "default", "level started at the saved checkpoint '" + level.start_point + "'" );
	report( "done" );
}

// God mode (on in QA: tools/qa_pair.sh sets mw2coop_god 1; see maps\_mw2coop::god_mode()):
// heavy damage downs nobody.
god()
{
	common_checks();
	check( maps\_mw2coop::god_mode_on(), "god mode on (mw2coop_god '" + GetDvar( "mw2coop_god" ) + "')" );
	wait 0.5;
	level.player2 DoDamage( level.player2.health + 500, level.player2.origin );
	level.player DoDamage( level.player.health + 500, level.player.origin );
	wait 1;
	check( IsAlive( level.player ) && IsAlive( level.player2 ), "both players alive after heavy damage" );
	check( !level.player ent_flag( "coop_downed" ) && !level.player2 ent_flag( "coop_downed" ), "neither player downed" );
	check( level.player.health > 0 && level.player2.health > 0, "health " + level.player.health + " / " + level.player2.health );
	report( "done" );
}

// weapnexttest (run on a level whose start gives two primaries, e.g. oilrig start "rig"): player 2 holds two weapons; the dvar that sync_weapons sets when his hands are empty is set by hand, and his weapon must change on its own (the client's DLL presses weapnext).
weapnexttest()
{
	common_checks();
	wait 3;
	p = level.player2;
	before = p GetCurrentWeapon();
	report( "weapnexttest: player 2 holds " + before + " of " + p GetWeaponsListPrimaries().size + " primaries" );
	SetDvar( "mw2coop_p2_weapnext", GetTime() );
	wait 3;
	after = p GetCurrentWeapon();
	check( after != before, "player 2's weapon changed from " + before + " to " + after + " after the weapon key was pressed for him" );
	report( "done" );
}

// hop: completes the mission 15 s after the level began. The dvar mw2coop_qa stays set, so every next level runs it again.
hop()
{
	common_checks();
	report( "hop: level " + level.script + " up, completing in 15 s" );
	wait 15;
	maps\_mw2coop::mission_complete();
	wait 60;
	report( "hop: still here after 60 s (the next mission did not start)" );
}

// falsefall (run with MW2COOP_PRE "set mw2coop_god 0;set bg_fallDamageMinHeight 10;set bg_fallDamageMaxHeight 30;set mw2coop_qa falsefall"; a script cannot set the fall dvars; split screen or pair): falling damage with no real fall behind it is undone (Vlad's Oil Rig log: player 2
// "landed" with 100 damage after 20 s in the air above the deck). A script cannot make a fall-less MOD_FALLING hit (DoDamage takes no means of death, and a teleport into the air gives the engine
// no start of the fall), so what is checked is that the guard does not undo a real fall: 300 units must still hurt. The undo itself is not reproduced yet.
falsefall()
{
	common_checks();
	wait 3;
	p = level.player2;
	spot = p.origin;
	level.mw2coop_fall_guard_off = true;
	p.health = p.maxhealth;
	p SetOrigin( spot + ( 0, 0, 25 ) );
	wait 1.5;
	report( "falsefall: guard off, 25 unit drop: health " + p.health );
	report( "falsefall: (info) the engine measures a fall from where the player left the ground: a teleport into the air gives it none, so this drop may cost nothing even without the guard" );
	level.mw2coop_fall_guard_off = false;
	p.health = p.maxhealth;
	wait 3;
	p SetOrigin( spot + ( 0, 0, 25 ) );
	wait 1.5;
	check( p.health >= p.maxhealth, "with the guard the same drop is undone (health " + p.health + ")" );
	p.health = p.maxhealth;
	wait 3;
	p SetOrigin( spot + ( 0, 0, 300 ) );
	wait 2;
	check( p.health < p.maxhealth || p ent_flag( "coop_downed" ), "a real fall of 300 units still hurts (health " + p.health + ")" );
	report( "done" );
}

// Downs on Recruit (run with MW2COOP_GOD=0): the difficulty comes from the players' profiles,
// so the scenario forces Recruit as the game's own level.forcedgameskill does. Player 2 is
// downed, then player 1 after the grace time: both down fails the mission.
downaudio()
{
	common_checks();
	level.forcedgameskill = 0;
	maps\_gameskill::setSkill( true );
	wait 3;
	victim = level.player2; // downp1: the host is the one who is downed (the video of 2026-11-12 showed the near-death filter, which player 2 never gets in this check)
	if ( GetDvar( "mw2coop_qa_variant" ) == "downp1" )
		victim = level.player;
	report( "downaudio: player 2 goes down" );
	if ( GetDvar( "mw2coop_qa_variant" ) != "nodown" ) // nodown: the control run, nobody is downed
		victim DoDamage( victim.health + 500, victim.origin );
	// mw2coop_qa_variant (audio checks): nobreath = no hurt breathing for player 2, noshock = no shellshock on him, noeq = no near-death EQ/reverb on him
	variant = GetDvar( "mw2coop_qa_variant" );
	if ( variant == "nobreath" )
		level.player2.gs.healthOverlayCutoff = -1;
	for ( i = 0; i < 20; i++ )
	{
		wait 1;
		if ( i == 4 && variant != "nodown" ) // a hit on the downed player: the stock scripts then start the dying effect (shellshock "default" for 60 s) on him
			victim DoDamage( 1, victim.origin );
		if ( variant == "noshock" )
			level.player2 StopShellShock();
		if ( variant == "noeq" )
		{
			level.player2 maps\_ambient::deactivate_index( level.eq_main_track );
			level.player2 maps\_ambient::deactivate_reverb();
		}
		report( "downaudio " + i + ": p1 red " + level.player ent_flag( "player_has_red_flashing_overlay" ) + " hp " + level.player.health + ", p2 red " + level.player2 ent_flag( "player_has_red_flashing_overlay" ) + " down " + level.player2 ent_flag( "coop_downed" ) + " hp " + level.player2.health );
	}
	level.player2 notify( "revive_success" );
	level.player2 ent_flag_clear( "coop_downed" );
	for ( i = 0; i < 6; i++ )
	{
		wait 1;
		report( "downaudio after " + i + ": p1 red " + level.player ent_flag( "player_has_red_flashing_overlay" ) + ", p2 red " + level.player2 ent_flag( "player_has_red_flashing_overlay" ) );
	}
	report( "done" );
}

// dmg (run with MW2COOP_PRE "set mw2coop_god 0;set mw2coop_qa dmg"): the player takes a hit every 4 s for a minute, to look at the damage overlays on the native renderers
dmg()
{
	wait 20;
	for ( i = 0; i < 15; i++ )
	{
		wait 4;
		report( "dmg " + i + ": hp " + level.player.health );
		level.player DoDamage( 25, level.player.origin + ( 100, 0, 40 ) );
	}
	report( "done" );
}

downs()
{
	common_checks();
	level.forcedgameskill = 0;
	maps\_gameskill::setSkill( true );
	wait 0.5;
	check( level.gameskill == 0, "difficulty Recruit (" + level.gameskill + ")" );
	check( isdefined( level.player_downed_death_buffer_time ), "time between downs defined on Recruit" );
	level.player2 DoDamage( level.player2.health + 500, level.player2.origin );
	wait 1;
	check( level.player2 ent_flag( "coop_downed" ) && !level.player ent_flag( "coop_downed" ), "player 2 downed, player 1 up" );
	wait 3;
	level.player DoDamage( level.player.health + 500, level.player.origin );
	wait 1;
	check( level.player ent_flag( "coop_downed" ), "player 1 downed after the grace time" );
	check( flag( "coop_alldowned" ) || level.mw2coop.failing, "both down: the mission fails" );
	report( "done" );
}

// A scene's values on the players survive a down and a revive (run with MW2COOP_GOD=0): levels
// set threatbias and attackeraccuracy for a scene (Loose Ends' accuracy ramp, Team Player's and
// The Only Easy Day's threatbias), and maps\_coop's resetSkill() on every down and revive put the
// difficulty's values back.
sceneskill()
{
	common_checks();
	foreach ( player in level.players )
	{
		player.threatbias = 777;
		player.attackeraccuracy = 0.123;
	}
	level.player2 DoDamage( level.player2.health + 500, level.player2.origin );
	wait 1.5;
	check( level.player2 ent_flag( "coop_downed" ), "player 2 downed" );
	check( level.player.threatbias == 777 && level.player.attackeraccuracy == 0.123, "player 1 keeps the scene's values after the down (" + level.player.threatbias + ", " + level.player.attackeraccuracy + ")" );
	level.player2 maps\_coop::player_coop_revive_self();
	wait 1.5;
	check( !level.player2 ent_flag( "coop_downed" ), "player 2 revived" );
	foreach ( n, player in level.players )
		check( player.threatbias == 777, "player " + ( n + 1 ) + " keeps the scene's threatbias after the revive (" + player.threatbias + ")" );
	check( level.player.attackeraccuracy == 0.123, "player 1 keeps the scene's attackeraccuracy after the revive (" + level.player.attackeraccuracy + ")" );
	report( "done" );
}

// The difficulty both machines play: each player's (player.gameskill, the host's pick in co-op)
// and the level's. mw2coop_qa_skill "<n>": the host's pick they must equal.
skill()
{
	common_checks();
	want = GetDvar( "mw2coop_qa_skill" );
	report( "skill: level " + level.gameskill + ", players " + level.player.gameskill + " " + level.player2.gameskill + ", profiles " + level.player GetPlayerSetting( "gameskill" ) + " " + level.player2 GetPlayerSetting( "gameskill" ) + ", mw2coop_skill '" + GetDvar( "mw2coop_skill" ) + "'" );
	if ( want != "" )
	{
		check( level.gameskill == int( want ), "level difficulty is the host's pick " + want + " (" + level.gameskill + ")" );
		foreach ( n, player in level.players )
			check( player.gameskill == int( want ), "player " + ( n + 1 ) + " plays difficulty " + want + " (" + player.gameskill + ")" );
	}
	report( "done" );
}

// Checkpoints from the level's sections (maps\_mw2coop::section_reached): the level passes into a
// new section, the mission fails, and both machines start again at that section. Run from a
// start point whose section ends by itself (Team Player "ride_later": the ride ends, "dismount"
// begins).
cprestart()
{
	want = "dismount";
	if ( GetDvar( "mw2coop_qa_phase" ) == "cp_failed" )
	{
		SetDvar( "mw2coop_qa_phase", "" );
		check( level.start_point == want, "after the failed mission the level starts at the checkpoint " + want + " (" + level.start_point + ")" );
		common_checks();
		report( "done" );
		return;
	}
	common_checks();
	for ( t = 0; t < 90 && GetDvar( "start" ) != want; t += 0.5 )
		wait 0.5;
	check( GetDvar( "start" ) == want, "checkpoint " + want + " reached (start '" + GetDvar( "start" ) + "')" );
	wait 2;
	SetDvar( "mw2coop_qa_phase", "cp_failed" );
	report( "failing the mission" );
	level thread maps\_mw2coop::mission_failed();
	wait 30;
	report( "FAIL no restart 30 s after the failure" );
}

// Friendly fire between players (run with MW2COOP_GOD=0; the lobby setting through
// MW2COOP_HOST_CMDS="set mw2coop_ff 0"): player 2 stands 80 units in front of player 1, who aims
// at his chest and fires for real; player 2's health before and after.
pvp()
{
	common_checks();
	wait 14;	// after the mission title
	foreach ( player in level.players )
	{
		player EnableWeapons();
		player FreezeControls( false );
	}
	fwd = AnglesToForward( ( 0, level.player GetPlayerAngles()[ 1 ], 0 ) );
	level.player2 SetOrigin( level.player.origin + fwd * 80 );
	wait 0.5;
	aim = VectorToAngles( ( level.player2.origin + ( 0, 0, 48 ) ) - level.player GetEye() );
	level.player SetPlayerAngles( aim );
	wait 0.5;
	before = level.player2.health;
	report( "player 1 weapon " + level.player GetCurrentWeapon() + ", g_friendlyfireDamageScale " + GetDvar( "g_friendlyfireDamageScale" ) + ", player 2 health " + before );
	press( level.player, "attack", 600 );
	wait 1.5;
	report( "player 2 health after player 1 fired: " + level.player2.health + " (was " + before + "), downed " + level.player2 ent_flag( "coop_downed" ) );
	report( "done" );
}

// The checkpoint table of the level (maps\_mw2coop_checkpoints): every flag in it exists.
cpcheck()
{
	common_checks();
	wait 5;
	pairs = maps\_mw2coop_checkpoints::table( level.script );
	report( level.script + ": " + pairs.size + " checkpoint flags" );
	foreach ( p in pairs )
		check( isdefined( level.flag[ p[ 0 ] ] ), "flag " + p[ 0 ] + " (" + p[ 1 ] + ") exists" );
	report( "done" );
}

// The Predator's UAV view for one player (maps\_remotemissile as patched: SetSavedDvar cg_fov 26
// plus maps\_mw2coop::fov_pilot): the pilot's screen zooms, the partner's keeps its field of
// view (src/fov.c). Player 1 is the pilot, then player 2. Frames on both machines; the DLL logs
// cg_fov (MW2COOP_QA_WATCH=cg_fov) and "fov: ... keeps".
pilotfov()
{
	common_checks();
	wait 14;	// after the mission title
	foreach ( n, pilot in level.players )
	{
		SetSavedDvar( "cg_fov", 26 );
		maps\_mw2coop::fov_pilot( pilot );
		report( "player " + ( n + 1 ) + " is the pilot (cg_fov 26)" );
		wait 1.5;
		qa_shot( "pilot" + ( n + 1 ) );
		SetSavedDvar( "cg_fov", 65 );
		maps\_mw2coop::fov_pilot( undefined );
		wait 1.5;
	}
	qa_shot( "after" );
	report( "done" );
}

// Team Player's ride (start point "ride", maps\_mw2coop_roadkill): the passenger sees "Hold [Use]
// to take the turret"; he holds Use for real, the two swap, and the new passenger sees it.
swaphint()
{
	common_checks();
	for ( t = 0; t < 30 && !( isdefined( level.mw2coop_roadkill_seat ) && isdefined( level.mw2coop_roadkill_seat.mw2coop_player ) ); t += 0.5 )
		wait 0.5;
	seat = level.mw2coop_roadkill_seat;
	check( isdefined( seat ) && isdefined( seat.mw2coop_player ), "a passenger is seated" );
	if ( !isdefined( seat ) || !isdefined( seat.mw2coop_player ) )
	{
		report( "done" );
		return;
	}
	wait 1;
	passenger = seat.mw2coop_player;
	gunner = maps\_mw2coop::vehicle_gunner();
	check( isdefined( passenger.mw2coop_swap_hint_elem ), player_name( passenger ) + " (passenger) sees the swap hint" );
	check( !isdefined( gunner ) || !isdefined( gunner.mw2coop_swap_hint_elem ), "the gunner does not" );
	qa_shot( "a-hint" );
	press( passenger, "activate", 1500 );
	wait 3;
	check( isdefined( level.player_turret ) && level.player_turret GetTurretOwner() == passenger, player_name( passenger ) + " took the turret" );
	now = seat.mw2coop_player;
	check( isdefined( now ) && now != passenger, "the other player is the passenger now" );
	if ( isdefined( now ) )
		check( isdefined( now.mw2coop_swap_hint_elem ), player_name( now ) + " (new passenger) sees the swap hint" );
	qa_shot( "b-swapped" );
	wait 7;
	check( !isdefined( passenger.mw2coop_swap_hint_elem ) && ( !isdefined( now ) || !isdefined( now.mw2coop_swap_hint_elem ) ), "the hint is gone after a few seconds" );
	report( "done" );
}

// Whiskey Hotel's roof (start point "flare", maps\_mw2coop_whitehouse): on "Use your flares!"
// each player holds a flare of his own; player 2 pops his with a real Attack press and player 1
// keeps his; then player 1 pops his.
// weapdrop (any level): a weapon player 2 dropped must not come back. Both players hold an AK-47 and an M1014 (player 1's, copied); player 2 drops the M1014 (TakeWeapon, as a pickup swap does);
// then player 1 picks up a new weapon, then a level script refills his ammo (GiveMaxAmmo on all his weapons: the mod mirrors it on player 2). Owner's report 2026-11-14: a client's dropped weapon
// came back whenever he picked up a new one.
weapdrop()
{
	common_checks();
	level.player GiveWeapon( "ak47" );
	level.player GiveWeapon( "m1014" );
	wait 2;
	report( "weapdrop: player 2 has " + weapdrop_text( level.player2 ) );
	check( level.player2 HasWeapon( "m1014" ), "player 2 got the M1014 from player 1" );
	level.player2 TakeWeapon( "m1014" );
	wait 1.5;
	check( !level.player2 HasWeapon( "m1014" ), "the dropped M1014 stays dropped for 1.5 s" );
	level.player GiveWeapon( "famas" );
	wait 2;
	report( "weapdrop: after player 1's new weapon, player 2 has " + weapdrop_text( level.player2 ) );
	check( level.player2 HasWeapon( "famas" ), "player 2 got the new weapon" );
	check( !level.player2 HasWeapon( "m1014" ), "the dropped M1014 did not come back with the new weapon" );
	foreach ( weapon in level.player GetWeaponsListAll() )
		level.player GiveMaxAmmo( weapon );
	wait 2;
	report( "weapdrop: after a refill of player 1's ammo, player 2 has " + weapdrop_text( level.player2 ) );
	check( !level.player2 HasWeapon( "m1014" ), "the dropped M1014 did not come back with a refill" );
	report( "done" );
}

weapdrop_text( player )
{
	text = "";
	foreach ( weapon in player GetWeaponsListAll() )
		text += weapon + "(" + player GetWeaponAmmoClip( weapon ) + "/" + player GetWeaponAmmoStock( weapon ) + ") ";
	return text;
}

// cpweapons (run with MW2COOP_MENU=1 MW2COOP_START=oval_office on dc_whitehouse: a start point that is not the default): weapons and ammunition survive a restart at a checkpoint. First run: player 1
// holds an AK-47 with 7 in the clip and 33 in stock plus the M1014, player 2 an AK-47 with 11 and 22 and no M1014; the checkpoint's weapons are saved (what the checkpoint code does), the ammo is spent
// (30/200), the mission fails. After the restart both must hold what they held at the checkpoint, not the level's loadout with full ammunition (owner's report 2026-11-14).
cpweapons()
{
	common_checks();
	if ( GetDvar( "mw2coop_qa_failed" ) == "1" )
	{
		wait 8;
		report( "cpweapons after the restart: player 1 ak47 " + level.player GetWeaponAmmoClip( "ak47" ) + "/" + level.player GetWeaponAmmoStock( "ak47" ) + " m1014 " + level.player HasWeapon( "m1014" ) + ", player 2 ak47 " + level.player2 GetWeaponAmmoClip( "ak47" ) + "/" + level.player2 GetWeaponAmmoStock( "ak47" ) + " m1014 " + level.player2 HasWeapon( "m1014" ) );
		check( level.player HasWeapon( "ak47" ) && level.player GetWeaponAmmoClip( "ak47" ) == 7 && level.player GetWeaponAmmoStock( "ak47" ) == 33, "player 1's AK-47 is back with 7/33" );
		check( level.player HasWeapon( "m1014" ), "player 1 still has the M1014" );
		check( level.player2 HasWeapon( "ak47" ) && level.player2 GetWeaponAmmoClip( "ak47" ) == 11 && level.player2 GetWeaponAmmoStock( "ak47" ) == 22, "player 2's AK-47 is back with 11/22" );
		check( !level.player2 HasWeapon( "m1014" ), "player 2 still does not have the M1014" );
		report( "done" );
		return;
	}
	wait 4;
	level.player GiveWeapon( "ak47" );
	level.player GiveWeapon( "m1014" );
	wait 2;
	level.player2 TakeWeapon( "m1014" );
	level.player SetWeaponAmmoClip( "ak47", 7 );
	level.player SetWeaponAmmoStock( "ak47", 33 );
	level.player2 SetWeaponAmmoClip( "ak47", 11 );
	level.player2 SetWeaponAmmoStock( "ak47", 22 );
	wait 1;
	maps\_mw2coop::snapshot_weapons( level.start_point );
	report( "cpweapons: saved " + GetDvar( "mw2coop_cp_w0" ) );
	report( "cpweapons: saved " + GetDvar( "mw2coop_cp_w1" ) );
	level.player SetWeaponAmmoClip( "ak47", 30 );
	level.player SetWeaponAmmoStock( "ak47", 200 );
	level.player2 SetWeaponAmmoClip( "ak47", 30 );
	level.player2 SetWeaponAmmoStock( "ak47", 200 );
	wait 1;
	SetDvar( "mw2coop_qa_failed", "1" );
	report( "cpweapons: failing the mission" );
	level thread maps\_mw2coop::mission_failed();
	wait 30;
}

// cpflags (estate, start point "house_defense", MW2COOP_PRE "set mw2coop_qa_failed 0"): the level's flags survive a restart at a checkpoint (maps\_mw2coop snapshot_flags /
// restore_flags). Owner 2026-11-17: after restarts at "escape" the objective marker stayed at the house (estate.gsc objectives() waited for flags that the start function of
// "escape" does not set). First run: the download is finished and the DSM taken (the level's own trigger), so the checkpoint "escape" is reached the normal way and keeps the
// flags; the mission fails. After the restart at "escape" every flag objectives() waits on must be set.
cpflags()
{
	common_checks();
	// (deploy_house_defense_jeeps and ghost_gives_regroup_order are not set by the start functions of house_defense or escape: a run that began there never had them; the estate
	// patch lets objectives() go on by the next step's flag instead. Read the objectives themselves with MW2COOP_MID "!objdump" after the restart.)
	need = strtok( "print_first_objective house_exterior_has_been_breached house_interior_breaches_done all_enemies_killed_up_to_house_capture house_briefing_is_over download_started download_complete dsm_recovered", " " );
	if ( GetDvar( "mw2coop_qa_failed" ) == "1" )
	{
		wait 3;
		report( "cpflags after the restart at '" + level.start_point + "': checkpoint kept " + GetDvar( "mw2coop_cp_fn" ) + " chunk(s) for '" + GetDvar( "mw2coop_cp_fname" ) + "'" );
		missing = "";
		foreach ( name in need )
			if ( !flag_exist( name ) || !flag( name ) )
				missing += name + " ";
		check( level.start_point == "escape", "the restart was at the checkpoint escape (" + level.start_point + ")" );
		check( missing == "", "the flags the objectives wait on are set again (missing: '" + missing + "')" );
		qa_shot( "afterrestart" );
		SetDvar( "mw2coop_qa_phase", "done" );
		report( "done" );
		return;
	}
	wait 5;
	missing = "";
	foreach ( name in need )
		if ( !flag_exist( name ) || !flag( name ) )
			missing += name + " ";
	report( "cpflags: at the start '" + level.start_point + "' these are clear: '" + missing + "'" );
	flag_set( "download_started" );
	wait 1;
	flag_set( "download_complete" );
	trig = GetEnt( "dsm_usetrigger", "targetname" );
	for ( t = 0; t < 15 && !flag( "dsm_recovered" ); t++ )
	{
		wait 1;
		trig notify( "trigger", level.player );
	}
	check( flag( "dsm_recovered" ), "the DSM was taken (" + t + " s)" );
	wait 2;	// the checkpoint (maps\_mw2coop_checkpoints: dsm_recovered marks "escape")
	report( "cpflags: checkpoint '" + GetDvar( "mw2coop_cp_fname" ) + "', " + level.mw2coop_cp_flags_kept + " flags kept in " + GetDvar( "mw2coop_cp_fn" ) + " chunk(s)" );
	check( GetDvar( "mw2coop_cp_fname" ) == "estate escape", "the checkpoint escape was reached and kept the flags" );
	SetDvar( "mw2coop_qa_failed", "1" );
	report( "cpflags: failing the mission" );
	level thread maps\_mw2coop::mission_failed();
	wait 30;
}

// nativesave (any level; split screen or network): the game's own save in a co-op session. A save is taken in memory (SaveGameNoCommit, CommitSave, as maps\_autosave
// does), then the world is changed (a flag set, both players moved, a level variable changed) and the mission fails (maps\_mw2coop::mission_failed): both machines must
// load the save (src/saves.c, fast_restart -> loadgame_continue_missionfailed). The save holds the script VM:
// this thread goes on after its wait in the saved state, finds the dvar mw2coop_qa_phase "loading" (dvars are not in a save) and checks that the flag is clear again, the
// variable has its old value and both players stand where they stood.
nativesave()
{
	common_checks();
	wait 4;
	if ( !flag_exist( "mw2coop_qa_sv" ) )
		flag_init( "mw2coop_qa_sv" );
	level.mw2coop_qa_marker = "before";
	spots = [];
	foreach ( i, player in level.players )
		spots[ i ] = player.origin;
	text = "";
	foreach ( i, spot in spots )
		text += " p" + ( i + 1 ) + " " + spot;
	report( "nativesave: " + level.players.size + " players," + text + ", time " + GetTime() );
	SetDvar( "mw2coop_qa_phase", "saving" );
	saveId = SaveGameNoCommit( "qasave", maps\_autosave::getDescription() );	// with its print, as the level's own autosaves
	report( "nativesave: SaveGameNoCommit -> " + saveId );
	wait 0.05;
	if ( GetDvar( "mw2coop_qa_phase" ) == "loading" )
	{
		SetDvar( "mw2coop_qa_phase", "loaded" );
		wait 2;
		report( "nativesave: after the load, time " + GetTime() + ", marker " + level.mw2coop_qa_marker + ", flag " + flag( "mw2coop_qa_sv" ) + ", players " + level.players.size );
		check( level.mw2coop_qa_marker == "before", "the level variable has its value from the save" );
		check( !flag( "mw2coop_qa_sv" ), "the flag set after the save is clear again" );
		foreach ( i, player in level.players )
			check( Distance( player.origin, spots[ i ] ) < 64, "player " + ( i + 1 ) + " is back where he stood at the save (" + Distance( player.origin, spots[ i ] ) + " units off)" );
		qa_shot( "afterload" );
		SetDvar( "mw2coop_qa_phase", "done" );
		report( "done" );
		return;
	}
	report( "nativesave: IsSaveRecentlyLoaded " + IsSaveRecentlyLoaded() + ", IsSaveSuccessful " + IsSaveSuccessful() );
	CommitSave( saveId );
	wait 0.4;
	qa_shot( "checkpoint" );	// the game's own "Checkpoint" text, if it shows in a co-op session
	wait 0.6;
	flag_set( "mw2coop_qa_sv" );
	level.mw2coop_qa_marker = "after";
	foreach ( player in level.players )
		player SetOrigin( player.origin + ( 0, 0, 2 ) + AnglesToForward( player.angles ) * 150 );
	wait 1;
	report( "nativesave: world changed, p1 " + level.players[ 0 ].origin + ", loading the save" );
	qa_shot( "beforeload" );
	SetDvar( "mw2coop_qa_phase", "loading" );
	level thread maps\_mw2coop::mission_failed();
	wait 25;
	report( "nativesave: no load 25 s after the mission failed" );
	check( false, "the save was loaded" );
}

// snapsave (two sessions; the host's MW2COOP_HOST_CMDS "set mw2coop_snapkey <key>", and in the second also "set mw2coop_snapload 1", MW2COOP_QA_CMDS
// "set mw2coop_qa_phase continued"): "Continue" from the game's checkpoint in a later session (src/saves.c snapshot_write / map_cmd). The first session commits
// a checkpoint (both PCs write their snapshot), then changes the world and ends. The second starts by loading the snapshot: this thread goes on after its wait
// in the restored state, finds the phase "continued" and checks that the world is the checkpoint's.
snapsave()
{
	common_checks();
	wait 4;
	if ( !flag_exist( "mw2coop_qa_sv" ) )
		flag_init( "mw2coop_qa_sv" );
	level.mw2coop_qa_marker = "snapshot";
	spots = [];
	foreach ( i, player in level.players )
		spots[ i ] = player.origin;
	saveId = SaveGameNoCommit( "qasnap", maps\_autosave::getDescription() );
	wait 0.05;
	if ( GetDvar( "mw2coop_qa_phase" ) == "continued" )
	{
		wait 2;
		report( "snapsave: continued session, time " + GetTime() + ", marker " + level.mw2coop_qa_marker + ", flag " + flag( "mw2coop_qa_sv" ) + ", players " + level.players.size );
		check( level.mw2coop_qa_marker == "snapshot", "the level variable has its value from the checkpoint" );
		check( !flag( "mw2coop_qa_sv" ), "the flag set after the checkpoint is clear" );
		foreach ( i, player in level.players )
			check( Distance( player.origin, spots[ i ] ) < 64, "player " + ( i + 1 ) + " stands where he stood at the checkpoint (" + Distance( player.origin, spots[ i ] ) + " units off)" );
		qa_shot( "continued" );
		SetDvar( "mw2coop_qa_phase", "done" );
		report( "done" );
		return;
	}
	CommitSave( saveId );
	wait 1;
	if ( flag_exist( "disable_autosaves" ) )
		flag_set( "disable_autosaves" );	// (the level's own next checkpoint would replace this one)
	flag_set( "mw2coop_qa_sv" );
	level.mw2coop_qa_marker = "after";
	foreach ( player in level.players )
		player SetOrigin( player.origin + ( 0, 0, 2 ) + AnglesToForward( player.angles ) * 150 );
	report( "snapsave: checkpoint committed and the world changed; the session can end" );
	wait 120;
}

// Just Like Old Times (af_caves, start "control"): the control room's slow-motion breach with both players in the door's volume and player 1 pressing Use for real
// (owner: "one of the two players stays stuck in the door after the breach"). Reports where each player is let go and whether he is stuck there
// (maps\_mw2coop::spot_stuck: no 8-unit step in any of four directions), then the same 5 s later, and whether player 1 can walk away (a real Forward press).
// mw2coop_qa_nounstick 1: the fix (maps\_mw2coop::breach_unstick) does nothing, to see the breach as the game leaves the players.
// mw2coop_qa_breachside 1: the players swap their places in the door's volume before the breach.
breachstuck()
{
	check( level.players.size == 2 && isdefined( level.player2 ), "two players (" + level.players.size + ")" );
	p1 = level.player;
	p2 = level.player2;
	level.mw2coop_no_follow = true;
	wait 3;
	if ( level.script == "af_caves" )
	{
		flag_set( "player_approaching_breach" );	// (the hallway's trigger, which the start point's teleport skips: an enemy slams the door, then the breach can be used)
		for ( t = 0; t < 15 && !flag( "breach_door_closed" ); t += 0.25 )
			wait 0.25;
		check( flag( "breach_door_closed" ), "the control room's door was slammed" );
		wait 2;
	}
	best = undefined;
	bestd = 1000000;
	foreach ( ent in level.breach_groups )
	{
		if ( !isdefined( ent.door_volume ) || !isdefined( ent.lookat_triggers ) || !ent.lookat_triggers.size )
			continue;
		d = Distance( p1.origin, ent.door_volume.origin );
		if ( d < bestd )
		{
			best = ent;
			bestd = d;
		}
	}
	check( isdefined( best ), "a breach near player 1 (" + bestd + " units)" );
	if ( !isdefined( best ) )
		return;
	door = best.lookat_triggers[ 0 ].breach_origin;
	vol = best.door_volume;
	inward = VectorNormalize( ( door[ 0 ] - vol.origin[ 0 ], door[ 1 ] - vol.origin[ 1 ], 0 ) );
	side = ( inward[ 1 ], inward[ 0 ] * -1, 0 );
	if ( GetDvar( "mw2coop_qa_breachside" ) == "1" )
		side = side * -1;
	report( "breachstuck: door at " + door + ", its volume at " + vol.origin + ", " + int( bestd ) + " units from player 1" );
	use_at = vol.origin;
	if ( isdefined( best.trigger ) )
		use_at = best.trigger.origin;
	spots = [];
	spots[ 0 ] = use_at + side * 14;
	spots[ 1 ] = use_at - side * 14;
	foreach ( n, player in level.players )
	{
		at = spots[ n ];
		player SetOrigin( PhysicsTrace( at + ( 0, 0, 40 ), at - ( 0, 0, 120 ) ) );
	}
	wait 0.5;
	foreach ( n, player in level.players )
		player SetPlayerAngles( VectorToAngles( door - player GetEye() ) );
	report( "breachstuck: player 1 in the door's volume " + p1 IsTouching( vol ) + ", player 2 " + p2 IsTouching( vol ) + "; player 1 in the use trigger " + ( isdefined( best.trigger ) && p1 IsTouching( best.trigger ) ) + ", " + int( Distance( p1.origin, door ) ) + " units from the door" );
	level.mw2coop_qa_breach_done = undefined;
	level thread breachstuck_watch();
	wait 0.5;
	p1 SetPlayerAngles( VectorToAngles( door - p1 GetEye() ) );
	press( p1, "activate", 1500 );
	for ( t = 0; t < 4 && !( isdefined( level.breaching ) && level.breaching ) && !isdefined( level.mw2coop_qa_breach_done ); t += 0.1 )
		wait 0.1;
	if ( !( isdefined( level.breaching ) && level.breaching ) && !isdefined( level.mw2coop_qa_breach_done ) && isdefined( best.trigger ) )
	{
		report( "breachstuck: player 1's Use did not start the breach: its trigger is notified for him" );
		best.trigger notify( "trigger", p1 );
	}
	for ( t = 0; t < 30 && !isdefined( level.mw2coop_qa_breach_done ); t += 0.1 )
		wait 0.1;
	check( isdefined( level.mw2coop_qa_breach_done ), "the breach ran (player 1's Use)" );
	if ( !isdefined( level.mw2coop_qa_breach_done ) )
		return;
	wait 0.1;
	foreach ( n, player in level.players )
		report( "breachstuck: let go, player " + ( n + 1 ) + " at " + player.origin + " linked " + player IsLinked() + " stuck " + maps\_mw2coop::spot_stuck( player.origin ) );
	qa_shot( "bs-release" );
	if ( GetDvar( "mw2coop_qa_nounstick" ) != "1" )
	{
		from = p1.origin;
		press( p1, "forward", 3000 );	// (pushing against what holds him: the fix should see it and move him into the room)
		wait 3;
		report( "breachstuck: player 1 pushed forward after the breach, now " + int( Distance( from, p1.origin ) ) + " units from where he was let go" );
	}
	wait 5;
	foreach ( n, player in level.players )
	{
		stuck = maps\_mw2coop::spot_stuck( player.origin );
		report( "breachstuck: 5 s later, player " + ( n + 1 ) + " at " + player.origin + " stuck " + stuck );
		check( !stuck, "player " + ( n + 1 ) + " can step after the breach" );
	}
	foreach ( k, yaw in StrTok( "0 90 180 270", " " ) )
	{
		from = p1.origin;
		p1 SetPlayerAngles( ( 0, int( yaw ), 0 ) );
		wait 0.2;
		press( p1, "forward", 800 );
		wait 1;
		report( "breachstuck: player 1 walked " + int( Distance( from, p1.origin ) ) + " units on Forward, facing " + yaw + " (speed scale unknown, slow motion " + ( isdefined( level.breaching ) && level.breaching ) + ")" );
	}
	qa_shot( "bs-after" );
	if ( GetDvar( "mw2coop_qa_nounstick" ) == "1" )
	{
		toward = VectorNormalize( ( p1.origin[ 0 ] - door[ 0 ], p1.origin[ 1 ] - door[ 1 ], 0 ) );
		foreach ( remedy in StrTok( "clip speed breaching up8 room40", " " ) )
		{
			if ( remedy == "clip" )
			{
				clip = GetEnt( "breach_solid", "targetname" );
				report( "breachstuck: the door's clip breach_solid " + isdefined( clip ) );
				if ( isdefined( clip ) )
					clip NotSolid();
			}
			else if ( remedy == "speed" )
				p1 SetMoveSpeedScale( 1 );
			else if ( remedy == "breaching" )
				p1 DisableBreaching();
			else if ( remedy == "up8" )
				p1 SetOrigin( p1.origin + ( 0, 0, 8 ) );
			else
				p1 SetOrigin( PhysicsTrace( p1.origin + toward * 40 + ( 0, 0, 40 ), p1.origin + toward * 40 - ( 0, 0, 80 ) ) );
			wait 0.3;
			from = p1.origin;
			p1 SetPlayerAngles( VectorToAngles( toward ) );
			wait 0.2;
			press( p1, "forward", 800 );
			wait 1;
			moved = int( Distance( from, p1.origin ) );
			report( "breachstuck: after '" + remedy + "' player 1 walked " + moved + " units" );
			if ( moved > 10 )
				break;
		}
	}
	if ( level.script == "af_caves" && !flag( "obj_door_controls_given" ) )
	{
		// the countdown on the control room's TVs (af_caves_backhalf breach_room_tvs: CinematicInGame): its draws are 3D quads the movie fitting must leave alone
		// (owner's screenshot: vertical strips in both halves once the countdown runs)
		flag_set( "obj_door_controls_given" );
		wait 5;
		room = best.room_volume.origin;	// (both look into the control room, at its TVs)
		foreach ( player in level.players )
			player SetPlayerAngles( VectorToAngles( ( room[ 0 ], room[ 1 ], player GetEye()[ 2 ] ) - player GetEye() ) );
		wait 1;
		qa_shot( "bs-countdown" );
		wait 3;
		qa_shot( "bs-countdown2" );
		wait 2;
		report( "breachstuck: the countdown runs on the TVs (frame bs-countdown)" );
	}
	report( "breachstuck: done" );
}

// A checkpoint that leads straight back to the failure (src/saves.c: the second load of the same checkpoint whose failure comes at the same game time restarts the
// level at its start point). The checkpoint is committed with a thread in it that fails the mission 2 s later, so every load fails again at the same moment, as
// Just Like Old Times' explosives did when they were shot a moment before a checkpoint (2026-11-18: twelve loads in a row). PASS when the level restarts at its start.
doomedcp()
{
	check( level.players.size == 2 && isdefined( level.player2 ), "two players (" + level.players.size + ")" );
	if ( GetDvar( "mw2coop_qa_phase" ) == "doomed" )
	{
		SetDvar( "mw2coop_qa_phase", "doomed_done" );
		check( true, "the level restarted at its start point after the checkpoint's failure came back" );
		report( "doomedcp: done" );
		return;
	}
	wait 4;
	SetDvar( "mw2coop_qa_phase", "doomed" );
	level thread doomedcp_fail();
	wait 0.05;
	saveId = SaveGameNoCommit( "qadoomed", maps\_autosave::getDescription() );
	wait 0.05;
	CommitSave( saveId );
	report( "doomedcp: checkpoint committed at game time " + GetTime() + " with a thread that fails the mission 2 s later" );
	wait 40;
	check( false, "the level restarted at its start point within 40 s" );
}

doomedcp_fail()
{
	wait 2;
	SetDvar( "ui_deadquote", "QA: the checkpoint's own failure" );
	level thread maps\_mw2coop::mission_failed();
}

breachstuck_watch()
{
	level waittill( "sp_slowmo_breachanim_done" );
	level.mw2coop_qa_breach_done = GetTime();
}

// whbreach (dc_whitehouse, run with MW2COOP_PRE "set start oval_office" for a restart there, or without it): the breach and the countdown. Reports the level's flags every 2 s for 16 s, then
// both players walk to the trigger of each flag in the level's order and the flags are reported again: after a restart at a checkpoint the breach scripts (whitehouse_entrance(), which starts the
// interior, the hammer-down jets and the flares) must run too (owner's report 2026-11-14: after a restart the countdown and the flares did not start).
whbreach()
{
	common_checks();
	names = strtok( "whitehouse_entrance_moveup whitehouse_entrance_init countdown_start whitehouse_breached whitehouse_kitchen_open whitehouse_interior whitehouse_hammerdown_jets whitehouse_flare_run", " " );
	report( "whbreach: start point '" + level.start_point + "'" );
	for ( t = 0; t < 16; t += 2 )
	{
		text = "";
		foreach ( name in names )
			text += name + "=" + flag( name ) + " ";
		report( "whbreach flags: " + text );
		wait 2;
	}
	friendly_before = GetDvar( "ai_friendlysuppression" );
	foreach ( name in strtok( "whitehouse_init whitehouse_moveout whitehouse_spotlight whitehouse_entrance_moveup whitehouse_entrance_init countdown_start whitehouse_breached whitehouse_kitchen_open", " " ) )
	{
		trigger = getEntWithFlag( name );
		if ( !isdefined( trigger ) )
		{
			report( "whbreach: no trigger sets " + name );
			continue;
		}
		if ( flag( name ) )
		{
			report( "whbreach: " + name + " is set already (trigger at " + trigger.origin + ")" );
			continue;
		}
		level.player SetOrigin( trigger.origin );
		level.player2 SetOrigin( trigger.origin + ( 40, 0, 0 ) );
		for ( t = 0; !flag( name ) && t < 6; t += 0.5 )
			wait 0.5;
		report( "whbreach: both moved to the trigger of " + name + " at " + trigger.origin + ": flag " + flag( name ) );
		wait 2;
	}
	wait 6;
	// test shortcut: Foley's kick of the kitchen door takes long with the bots; the interior part (whitehouse_interior(), started by whitehouse_entrance() after the door) follows it
	if ( !flag( "whitehouse_kitchen_open" ) )
		flag_set( "whitehouse_kitchen_open" );
	wait 3;
	report( "whbreach: ai_friendlysuppression " + friendly_before + " -> " + GetDvar( "ai_friendlysuppression" ) + " (whitehouse_interior() sets it to 0)" );
	text = "";
	foreach ( name in names )
		text += name + "=" + flag( name ) + " ";
	report( "whbreach final flags: " + text );
	check( GetDvar( "ai_friendlysuppression" ) == "0", "the interior part ran after the breach: the hammer-down jets and the flares are started by it (whitehouse_entrance -> whitehouse_interior -> whitehouse_flare)" );
	check( flag( "countdown_start" ), "the countdown started" );
	check( flag( "whitehouse_interior" ), "the interior part of the level started (whitehouse_radio sets it with the countdown)" );
	check( isdefined( level.hammerdown_time ), "the hammer-down timer runs (whitehouse_interior ran)" );
	report( "done" );
}

flares()
{
	common_checks();
	// the way the level goes: the breach, then the roof (the bots do not walk there)
	foreach ( name in strtok( "whitehouse_flare_breach whitehouse_flare_run whitehouse_path_roof", " " ) )
	{
		trigger = getEntWithFlag( name );
		if ( !isdefined( trigger ) || flag( name ) )
			continue;
		level.player SetOrigin( trigger.origin );
		level.player2 SetOrigin( trigger.origin + ( 40, 0, 0 ) );
		report( "both moved to the trigger of " + name );
		for ( t = 0; !flag( name ) && t < 6; t += 0.5 )
			wait 0.5;
	}
	for ( t = 0; !flag( "player_flare" ) && t < 40; t += 0.5 )
		wait 0.5;
	if ( !flag( "player_flare" ) )
	{
		// Foley's flare scene did not reach the roof with the bots: start the jets as it would
		report( "test shortcut: the jets flag set by the test (Foley's scene did not get there)" );
		flag_set( "whitehouse_flare_run" );
		flag_set( "whitehouse_hammerdown_jets_fly" );
		for ( t = 0; !flag( "player_flare" ) && t < 10; t += 0.5 )
			wait 0.5;
	}
	check( flag( "player_flare" ), "Foley calls for the flares" );
	wait 1.5;
	foreach ( n, player in level.players )
		check( player HasWeapon( "flare" ) && player GetCurrentWeapon() == "flare", "player " + ( n + 1 ) + " holds a flare (" + player GetCurrentWeapon() + ")" );
	qa_shot( "a-flares" );
	press( level.player2, "attack", 300 );
	wait 12;	// lighting and waving the flare, then it is taken
	qa_shot( "b-player2-popped" );
	check( !level.player2 HasWeapon( "flare" ), "player 2's flare is used up" );
	check( level.player HasWeapon( "flare" ), "player 1 still has his flare" );
	check( flag( "player_flare_popped" ), "the flare hint is done" );
	press( level.player, "attack", 300 );
	wait 12;
	qa_shot( "c-player1-popped" );
	check( !level.player HasWeapon( "flare" ), "player 1's flare is used up" );
	foreach ( n, player in level.players )
		check( player GetCurrentWeapon() != "flare" && player GetCurrentWeapon() != "none", "player " + ( n + 1 ) + " back on a weapon (" + player GetCurrentWeapon() + ")" );
	report( "done" );
}

// Passenger ride (Wolverines! opening: start point default on invasion): player 2 rides on a
// free seat of player 1's vehicle and gets out with him.
ride()
{
	check( level.players.size == 2 && isdefined( level.player2 ), "two players (" + level.players.size + ")" );
	t = 0;
	while ( !level.player IsLinked() && t < 30 )
	{
		wait 0.5;
		t += 0.5;
	}
	check( level.player IsLinked(), "player 1 seated" );
	wait 1.5;
	check( level.player2 IsLinked(), "player 2 seated too" );
	check( Distance( level.player.origin, level.player2.origin ) < 120, "player 2 in the same vehicle (" + int( Distance( level.player.origin, level.player2.origin ) ) + " units)" );
	wait 6;
	check( Distance( level.player.origin, level.player2.origin ) < 120, "still together while driving (" + int( Distance( level.player.origin, level.player2.origin ) ) + " units)" );
	t = 0;
	while ( level.player IsLinked() && t < 120 )
	{
		wait 0.5;
		t += 0.5;
	}
	check( !level.player IsLinked(), "player 1 got out" );
	wait 1.5;
	check( !level.player2 IsLinked(), "player 2 got out" );
	check( Distance( level.player.origin, level.player2.origin ) < 160, "player 2 next to player 1 (" + int( Distance( level.player.origin, level.player2.origin ) ) + " units)" );
	report( "done" );
}

// Turret hand-over experiment (roadkill, start point "ride": player 1 starts on the Humvee
// turret). Takes player 1 off with a second UseBy and puts player 2 on.
// Team Player's passenger seat: what player 2 sees from the rear seat while player 1 is the gunner
// (frames of both machines in mw2coop\shots; the client's is player 2's).
seatview()
{
	check( level.players.size == 2 && isdefined( level.player2 ), "two players (" + level.players.size + ")" );
	t = 0;
	while ( !( isdefined( level.player_turret ) && isdefined( level.player_turret GetTurretOwner() ) ) && t < 30 )
	{
		wait 0.5;
		t += 0.5;
	}
	wait 6;
	report( "gunner " + ( level.player_turret GetTurretOwner() == level.player ) + ", player 2 linked " + level.player2 IsLinked() + " at " + level.player2.origin + " angles " + level.player2 GetPlayerAngles() + ", humvee " + level.player_humvee.origin );
	yaw = AngleClamp180( level.player2 GetPlayerAngles()[ 1 ] - level.player_humvee.angles[ 1 ] );
	check( Abs( yaw ) < 40, "player 2 starts looking ahead (" + int( yaw ) + " degrees off the Humvee's heading)" );
	hp = level.player.health;
	level.player DoDamage( 30, level.player.origin, level.player2, level.player2, "MOD_RIFLE_BULLET" );
	wait 0.5;
	check( level.player.health >= hp, "player 2's bullets do not hurt the gunner (" + hp + " -> " + level.player.health + ")" );
	qa_shot( "seat-front" );
	level.player2 SetPlayerAngles( level.player2 GetPlayerAngles() + ( 0, 90, 0 ) );
	wait 0.5;
	qa_shot( "seat-side" );
	level.player2 SetPlayerAngles( level.player2 GetPlayerAngles() + ( 0, 90, 0 ) );
	wait 0.5;
	qa_shot( "seat-back" );
	report( "done" );
}

turret()
{
	check( level.players.size == 2 && isdefined( level.player2 ), "two players (" + level.players.size + ")" );
	t = 0;
	while ( !isdefined( level.player_turret ) && t < 20 )
	{
		wait 0.5;
		t += 0.5;
	}
	turret = level.player_turret;
	check( isdefined( turret ), "player turret exists" );
	wait 2;
	check( turret GetTurretOwner() == level.player, "player 1 is the gunner" );

	level.player EnableTurretDismount();
	turret UseBy( level.player );
	wait 0.5;
	owner = turret GetTurretOwner();
	check( !isdefined( owner ), "second UseBy takes player 1 off the turret" );
	report( "player 1 using a turret: " + level.player IsUsingTurret() + ", linked: " + level.player IsLinked() );

	level.player2 PlayerLinkToDelta( level.player_humvee, "tag_player", 0.35, 360, 360, 45, 30, true );
	turret MakeUsable();
	turret UseBy( level.player2 );
	turret MakeUnusable();
	level.player2 DisableTurretDismount();
	wait 0.5;
	check( turret GetTurretOwner() == level.player2, "player 2 is the gunner now" );
	report( "player 2 using a turret: " + level.player2 IsUsingTurret() + ", linked: " + level.player2 IsLinked() );
	wait 3;
	check( turret GetTurretOwner() == level.player2, "player 2 still on the turret after 3 s" );
	report( "done" );
}

// Team Player boarding (roadkill, start point "convoy"): player 2 reaches the seat first and
// becomes the gunner, player 1 boards the rear seat, the convoy leaves, the seats swap and swap
// back (called directly: a script cannot hold the Use button), both stay in the moving Humvee.
board()
{
	check( level.players.size == 2 && isdefined( level.player2 ), "two players (" + level.players.size + ")" );
	t = 0;
	while ( !isdefined( level.chair ) && t < 60 )
	{
		wait 0.25;
		t += 0.25;
	}
	check( isdefined( level.chair ), "glowing seat spawned" );
	flag_set( "player_climbs_stairs" );	// a trigger player 1 would touch on the stairs: the convoy moment
	// player 2 waits at the seat until someone boards
	t = 0;
	while ( !isdefined( level.mw2coop_gunner ) && t < 90 )
	{
		if ( isdefined( level.chair ) )
			level.player2 SetOrigin( level.chair.origin + ( 0, 0, -10 ) );
		wait 0.1;
		t += 0.1;
	}
	check( isdefined( level.mw2coop_gunner ) && level.mw2coop_gunner == level.player2, "player 2 boarded first and is the gunner" );
	t = 0;
	while ( !( isdefined( level.player_turret ) && isdefined( level.player_turret GetTurretOwner() ) ) && t < 15 )
	{
		wait 0.25;
		t += 0.25;
	}
	check( isdefined( level.player_turret ) && level.player_turret GetTurretOwner() == level.player2, "player 2 on the turret" );
	check( !flag( "player_gets_in" ), "convoy waits for player 1" );
	wait 1;
	level.player SetOrigin( level.player_humvee GetTagOrigin( "tag_seat_rb_attach" ) + ( 0, 0, -10 ) );
	wait 1;
	check( level.player IsLinked(), "player 1 boarded the rear seat" );
	check( flag( "player_gets_in" ), "convoy leaves once both are in" );
	wait 2;

	level.player_humvee maps\_mw2coop_roadkill::swap( level.mw2coop_roadkill_seat );
	wait 1;
	check( level.player_turret GetTurretOwner() == level.player && level.player2 IsLinked(), "swap: player 1 gunner, player 2 seated" );
	level.player_humvee maps\_mw2coop_roadkill::swap( level.mw2coop_roadkill_seat );
	wait 1;
	check( level.player_turret GetTurretOwner() == level.player2 && level.player IsLinked(), "swap back: player 2 gunner, player 1 seated" );

	wait 6;
	d = Distance( level.player.origin, level.player2.origin );
	check( d < 150 && level.player IsLinked() && level.player2 IsLinked(), "both riding the moving Humvee (" + int( d ) + " units apart)" );
	report( "done" );
}

// Team Player ride end (roadkill, start point "ride_later": player 1 starts on the turret):
// player 2 rides on a seat, both get off when the ride ends, player 2 lands next to player 1.
rideend()
{
	check( level.players.size == 2 && isdefined( level.player2 ), "two players (" + level.players.size + ")" );
	wait 3;
	check( isdefined( level.player_turret ) && level.player_turret GetTurretOwner() == level.player, "player 1 on the turret" );
	check( Distance( level.player.origin, level.player_turret.origin ) < 150, "player 1 at the turret (" + int( Distance( level.player.origin, level.player_turret.origin ) ) + " units)" );
	check( level.player2 IsLinked(), "player 2 seated" );
	report( "t 3: player 1 " + int( Distance( level.player.origin, level.player_turret.origin ) ) + " units from the turret, player 2 " + int( Distance( level.player2.origin, level.player_turret.origin ) ) );
	t = 0;
	while ( !flag( "player_is_dismounted" ) && t < 180 )
	{
		wait 0.5;
		t += 0.5;
		if ( t == int( t / 2 ) * 2 && t <= 12 || t == int( t / 10 ) * 10 )
			report( "t " + t + ": turret " + level.player IsUsingTurret() + ", " + int( Distance( level.player.origin, level.player2.origin ) ) + " units, linked " + level.player IsLinked() + "/" + level.player2 IsLinked()
				+ ", flags " + flag_text( "lead_vehicle_speeds_up" ) + flag_text( "resume_the_path" ) + flag_text( "player_is_pushing_truck" ) + flag_text( "player_goes_in_reverse" ) + flag_text( "player_vehicle_wipes_out" ) );
	}
	check( flag( "player_is_dismounted" ), "ride ended in the crash (" + t + " s)" );
	wait 1;
	check( !level.player IsLinked() && !level.player2 IsLinked(), "both off the Humvee" );
	d = Distance( level.player.origin, level.player2.origin );
	check( d < 200, "player 2 next to player 1 after the crash (" + int( d ) + " units)" );
	report( "done" );
}

flag_text( name )
{
	if ( !isdefined( level.flag[ name ] ) )
		return "?";
	if ( flag( name ) )
		return "1";
	return "0";
}

// QA probe, started from maps\_mw2coop::main(): when player 1's link changes in the first 3 s.
link_probe()
{
	text = "";
	last = -1;
	for ( i = 0; i < 60; i++ )
	{
		now = 0;
		if ( level.player IsLinked() )
			now = 1;
		if ( i == 0 && isdefined( level.player2 ) )
			text += " (player 2 at frame 0 @" + int( level.player2.origin[ 0 ] ) + "," + int( level.player2.origin[ 1 ] ) + "," + int( level.player2.origin[ 2 ] ) + ")";
		if ( now != last )
			text += " frame " + i + ": " + now + " @" + int( level.player.origin[ 0 ] ) + "," + int( level.player.origin[ 1 ] );
		last = now;
		wait 0.05;
	}
	if ( isdefined( level.player2 ) )
		text += "; player 2 now @" + int( level.player2.origin[ 0 ] ) + "," + int( level.player2.origin[ 1 ] ) + "," + int( level.player2.origin[ 2 ] ) + " linked " + level.player2 IsLinked();
	maps\_mw2coop::state( "link probe:" + text );
}

// QA probe, started from maps\_mw2coop::main() in the Museum: the dioramas' animations (the .animation of the structs the drones stand on)
// that level.scr_anim[ "generic" ] has no entry for. On iw4x-sp the Museum's setup failed about 40 times with "type undefined is not an
// anim" where the 64-bit game had one (night tour 2026-11-18/19).
museum_anim_probe()
{
	if ( level.script != "ending" || !isdefined( level.struct ) )
		return;
	wait 1;
	missing = [];
	with = 0;
	foreach ( s in level.struct )
	{
		if ( !isdefined( s.animation ) )
			continue;
		with++;
		if ( isdefined( level.scr_anim[ "generic" ][ s.animation ] ) || isdefined( missing[ s.animation ] ) )
			continue;
		missing[ s.animation ] = 1;
		maps\_mw2coop::state( "museum anim probe: no generic anim " + s.animation + " (" + s.origin + ")" );
		wait 0.05;	// one state line a frame reaches the log
	}
	maps\_mw2coop::state( "museum anim probe: " + with + " structs with an animation, " + missing.size + " names without a generic anim" );
}

// Team Player opening (roadkill, default start): during Shepherd's "get on the line" scene both
// players are on the view rig; afterwards both are free and together.
rkintro()
{
	check( level.players.size == 2 && isdefined( level.player2 ), "two players (" + level.players.size + ")" );
	t = 0;
	both = false;
	while ( !flag( "get_on_the_line" ) && t < 90 )
	{
		if ( level.player IsLinked() && level.player2 IsLinked() )
			both = true;
		wait 0.25;
		t += 0.25;
	}
	check( flag( "get_on_the_line" ), "Shepherd's scene ended (" + t + " s)" );
	check( both, "both players were in the scene" );
	wait 1;
	check( !level.player IsLinked() && !level.player2 IsLinked(), "both free after the scene" );
	d = Distance( level.player.origin, level.player2.origin );
	check( d < 200, "player 2 next to player 1 (" + int( d ) + " units)" );
	report( "done" );
}

// Leash (S.S.D.D.): player 2 is put 4000 units away; within leash_time() + 2 s he is back
// next to player 1.
leash()
{
	common_checks();
	level.player2 SetOrigin( level.player.origin + ( 4000, 0, 0 ) );
	wait 1;
	check( Distance( level.player.origin, level.player2.origin ) > 3000, "player 2 is far away (" + int( Distance( level.player.origin, level.player2.origin ) ) + " units)" );
	wait maps\_mw2coop::leash_time() + 1;
	d = Distance( level.player.origin, level.player2.origin );
	check( d < 200, "leash brought player 2 back (" + int( d ) + " units)" );
	report( "done" );
}

// Cliffhanger snowmobiles (start point "snowmobile"): each player on his own snowmobile; at the
// helicopter the ending waits while player 2 is still on his snowmobile and goes on once he is
// off next to player 1. (A script cannot drive, so the ride itself is skipped by setting the
// level's flags.)
snow()
{
	check( level.players.size == 2 && isdefined( level.player2 ), "two players (" + level.players.size + ")" );
	wait 3;
	check( isdefined( level.player.vehicle ), "player 1 on a snowmobile" );
	check( isdefined( level.player2.vehicle ) && ( !isdefined( level.player.vehicle ) || level.player2.vehicle != level.player.vehicle ), "player 2 on his own snowmobile" );
	foreach ( name in strtok( "player_reaches_hilltop there_is_chopper ending_heli_flies_in", " " ) )
	{
		qa_flag_set( name );
		wait 1;
	}
	level.player player_dismount_vehicle();
	wait 8;	// the radio lines between the flags
	SetDvar( "mw2coop_qa_phase", "done" );
	qa_flag_set( "end_begins" );
	wait 5;
	check( !level.mw2coop.completing, "ending waits while player 2 is on his snowmobile" );
	level.player2 player_dismount_vehicle();
	wait 1;
	level.player2 maps\_mw2coop::teleport_to_partner( level.player );
	t = 0;
	while ( !level.mw2coop.completing && t < 15 )
	{
		wait 0.25;
		t += 0.25;
	}
	check( level.mw2coop.completing, "mission ends once player 2 is there (" + t + " s)" );
	report( "done" );
}

qa_flag_set( name )
{
	if ( !isdefined( level.flag[ name ] ) )
		flag_init( name );
	flag_set( name );
}

// self: a snowmobile ridden in first person: where its attached hands hold the bars
report_grips()
{
	seat = self GetTagOrigin( "tag_driver" );
	fwd = AnglesToForward( self.angles );
	right = AnglesToRight( self.angles );
	text = "grips:";
	foreach ( tag in strtok( "j_wrist_ri j_wrist_le", " " ) )
	{
		d = self GetTagOrigin( tag ) - seat;
		text += " " + tag + " " + int( VectorDot( d, fwd ) ) + "/" + int( VectorDot( d, right ) ) + "/" + int( d[ 2 ] );
	}
	report( text );
}

// "head f/r/u ..." of a rider body: its bones from the vehicle's tag_driver, along the vehicle
rider_bones( r, v )
{
	seat = v GetTagOrigin( "tag_driver" );
	fwd = AnglesToForward( v.angles );
	right = AnglesToRight( v.angles );
	text = "";
	foreach ( tag in strtok( "j_head j_spine4 j_hip_le j_wrist_ri j_wrist_le j_knee_le j_ankle_le", " " ) )
	{
		d = r GetTagOrigin( tag ) - seat;
		text += tag + " " + int( VectorDot( d, fwd ) ) + "/" + int( VectorDot( d, right ) ) + "/" + int( d[ 2 ] ) + "  ";
	}
	return text;
}

// The Gulag's cellblock rope (start "rappel"): player 1 uses it and rappels, the rope hangs ready
// again, player 2 uses it and rappels; both end up below, not linked, with their weapons.
rappel()
{
	check( level.players.size == 2 && isdefined( level.player2 ), "two players (" + level.players.size + ")" );
	if ( level.script == "af_caves" )
	{
		cave_rappel();
		return;
	}
	wait 4;
	trigger = GetEnt( "rappel_trigger", "script_noteworthy" );
	check( isdefined( trigger ), "the rope's use trigger exists" );
	top = level.player.origin;
	level.player2 SetOrigin( level.player.origin + ( 30, 0, 0 ) );
	wait 1;
	trigger notify( "trigger", level.player );
	wait 3;
	check( level.player IsLinked(), "player 1 is on the rope" );
	level.player2 SetOrigin( level.player.origin + ( 30, 0, 0 ) );
	wait 12;
	d1 = top[ 2 ] - level.player.origin[ 2 ];
	check( !level.player IsLinked() && d1 > 100, "player 1 came down and was let go (" + int( d1 ) + " units)" );
	check( isdefined( GetEnt( "rappel_trigger", "script_noteworthy" ) ), "the rope is usable again for player 2" );
	level.player2 SetOrigin( top + ( 20, 0, 0 ) );
	wait 1;
	trigger notify( "trigger", level.player2 );
	wait 3;
	check( level.player2 IsLinked(), "player 2 is on the rope" );
	wait 12;
	d2 = top[ 2 ] - level.player2.origin[ 2 ];
	check( !level.player2 IsLinked() && d2 > 100, "player 2 came down and was let go (" + int( d2 ) + " units)" );
	d = Distance( level.player.origin, level.player2.origin );
	report( "at the bottom: " + int( d ) + " units apart" );
	report( "player 1 holds " + level.player GetCurrentWeapon() + ", player 2 holds " + level.player2 GetCurrentWeapon() );
	report( "done" );
}

// Just Like Old Times (start "rappel"): player 1 hooks up, rappels and kills the guard; the hook-up is
// free again, player 2 hooks up and rappels; both end up below, not linked, with their weapons.
cave_rappel()
{
	t = 0;
	while ( !flag( "price_hooksup" ) && t < 60 )
	{
		wait 0.5;
		t += 0.5;
	}
	check( flag( "price_hooksup" ), "Price hooked up" );
	wait 2;
	trigger = GetEnt( "player_rappel_trigger", "targetname" );
	check( isdefined( trigger ), "the hook-up's use trigger exists" );
	top = level.player.origin;
	level.player2 SetOrigin( top + ( 40, 0, 0 ) );
	wait 1;
	trigger notify( "trigger", level.player );
	wait 3;
	check( level.player IsLinked(), "player 1 is on the rope" );
	t = 0;
	while ( !flag( "end_of_rappel_scene" ) && t < 90 )
	{
		if ( flag( "rappel_end" ) && !flag( "player_killing_guard" ) )
			press( level.player, "melee", 500 );
		wait 0.5;
		t += 0.5;
	}
	check( flag( "player_killing_guard" ), "player 1 killed the guard" );
	check( flag( "end_of_rappel_scene" ), "player 1's scene ended (" + t + " s)" );
	wait 6;
	d1 = top[ 2 ] - level.player.origin[ 2 ];
	check( !level.player IsLinked() && d1 > 100, "player 1 came down and was let go (" + int( d1 ) + " units)" );
	check( isdefined( trigger ), "the hook-up is usable again for player 2" );
	// the barracks' "abandoned Price" check fails the mission for a player 1 who stays at the bottom
	if ( isdefined( level.flag[ "barracks_stealthbreak_survived" ] ) )
		flag_set( "barracks_stealthbreak_survived" );
	bottom1 = level.player.origin;
	level.player2 SetOrigin( top + ( 40, 0, 0 ) );
	wait 1;
	trigger notify( "trigger", level.player2 );
	wait 3;
	check( level.player2 IsLinked(), "player 2 is on the rope" );
	t = 0;
	while ( level.player2 IsLinked() && t < 90 )
	{
		wait 0.5;
		t += 0.5;
	}
	wait 2;
	d2 = top[ 2 ] - level.player2.origin[ 2 ];
	check( !level.player2 IsLinked() && d2 > 100, "player 2 came down and was let go (" + int( d2 ) + " units, " + t + " s)" );
	check( !level.player IsLinked() && Distance( level.player.origin, bottom1 ) < 200, "player 1 stayed at the bottom while player 2 rappelled (" + int( Distance( level.player.origin, bottom1 ) ) + " units)" );
	report( "at the bottom: " + int( Distance( level.player.origin, level.player2.origin ) ) + " units apart" );
	report( "player 1 holds " + level.player GetCurrentWeapon() + ", player 2 holds " + level.player2 GetCurrentWeapon() );
	report( "done" );
}

// Just Like Old Times (start "rappel"), the rope as the partner sees it: the one who waits rides a camera beside and behind the one on the rope
// (linked to a point that follows him, looking at him); frames of both screens every second from the hook-up to the end of each descent.
rappelview()
{
	if ( level.script == "gulag" )
	{
		gulag_rappelview();
		return;
	}
	check( level.script == "af_caves" && level.players.size == 2, "af_caves with two players" );
	t = 0;
	while ( !flag( "price_hooksup" ) && t < 60 )
	{
		wait 0.5;
		t += 0.5;
	}
	wait 2;
	trigger = GetEnt( "player_rappel_trigger", "targetname" );
	top = level.player.origin;
	level.player2 SetOrigin( top + ( 40, 0, 0 ) );
	wait 1;
	trigger notify( "trigger", level.player );
	level.player2 thread qa_side_camera( level.player );
	level thread qa_shots_every( "p1-", 1.0, 40 );
	t = 0;
	while ( !flag( "end_of_rappel_scene" ) && t < 90 )
	{
		if ( flag( "rappel_end" ) && !flag( "player_killing_guard" ) )
			press( level.player, "melee", 500 );
		wait 0.5;
		t += 0.5;
	}
	level notify( "qa_shots_stop" );
	level.player2 notify( "qa_look_stop" );
	check( flag( "end_of_rappel_scene" ), "player 1's scene ended (" + t + " s)" );
	wait 4;
	if ( isdefined( level.flag[ "barracks_stealthbreak_survived" ] ) )
		flag_set( "barracks_stealthbreak_survived" );
	level.player2 SetOrigin( top + ( 40, 0, 0 ) );
	wait 1;
	trigger notify( "trigger", level.player2 );
	level.player thread qa_side_camera( level.player2 );
	level thread qa_shots_every( "p2-", 1.0, 30 );
	t = 0;
	while ( !level.player2 IsLinked() && t < 5 )
	{
		wait 0.25;
		t += 0.25;
	}
	check( level.player2 IsLinked(), "player 2 is on the rope" );
	t = 0;
	while ( level.player2 IsLinked() && t < 90 )
	{
		wait 0.5;
		t += 0.5;
	}
	level notify( "qa_shots_stop" );
	level.player notify( "qa_look_stop" );
	report( "done" );
}

// The Gulag's cellblock rope (start "rappel"), seen by the partner: player 1 rappels, then player 2; the other rides a camera beside him.
gulag_rappelview()
{
	check( level.players.size == 2, "two players" );
	wait 4;
	trigger = GetEnt( "rappel_trigger", "script_noteworthy" );
	check( isdefined( trigger ), "the rope's use trigger exists" );
	top = level.player.origin;
	level.player2 SetOrigin( top + ( 30, 0, 0 ) );
	wait 1;
	trigger notify( "trigger", level.player );
	level.player2 thread qa_side_camera( level.player );
	level thread qa_shots_every( "g1-", 0.7, 18 );
	wait 13;
	level notify( "qa_shots_stop" );
	level.player2 notify( "qa_look_stop" );
	wait 1;
	level.player2 SetOrigin( top + ( 20, 0, 0 ) );
	wait 1;
	trigger = GetEnt( "rappel_trigger", "script_noteworthy" );
	check( isdefined( trigger ), "the rope is usable again for player 2" );
	if ( isdefined( trigger ) )
		trigger notify( "trigger", level.player2 );
	level.player thread qa_side_camera( level.player2 );
	level thread qa_shots_every( "g2-", 0.7, 18 );
	wait 13;
	level notify( "qa_shots_stop" );
	level.player notify( "qa_look_stop" );
	report( "done" );
}

// Big Brother (so_chopper_invasion): the gunner's angles on the server, once a second (Vlad: with player 2 on the keyboard and mouse the turret is gone and the
// camera does not turn; on the client his view stays level while it should look down the gun)
bbcheck()
{
	for ( n = 0; n < 20; n++ )
	{
		wait 1;
		foreach ( i, player in level.players )
		{
			gunner = "";
			if ( isdefined( level.chopperplayer ) && level.chopperplayer == player )
				gunner = " (gunner)";
			maps\_mw2coop::state( "bbcheck " + "t" + n + " player " + ( i + 1 ) + gunner + ": angles " + player GetPlayerAngles() + " linked " + player IsLinked() + " origin " + vec_int( player.origin ) );
			wait 0.3;	// (state() is a dvar the DLL reads every 250 ms)
		}
		if ( isdefined( level.chopper ) )
		{
			user = "none";
			if ( isdefined( level.chopper.minigunUser ) )
			{
				user = "player 1";
				if ( level.chopper.minigunUser == level.player2 )
					user = "player 2";
			}
			maps\_mw2coop::state( "bbcheck " + "t" + n + " chopper minigunUser " + user + " tag_player " + level.chopper GetTagAngles( "tag_player" ) );
		}
	}
	maps\_mw2coop::state( "bbcheck " + "done" );
}

// frames of both screens every <step> seconds until "qa_shots_stop" (at most max of them)
qa_shots_every( prefix, step, max )
{
	level endon( "qa_shots_stop" );
	for ( n = 0; n < max; n++ )
	{
		qa_shot( prefix + n );
		wait step;
	}
}

qa_look_done_on_stop()
{
	self waittill( "qa_look_stop" );
	self.qa_look_done = true;
}

// self: a player who rides a camera point beside and behind target (away from what target faces), looking at target's eye, until "qa_look_stop";
// then he is let go where he was.
qa_side_camera( target )
{
	cam = Spawn( "script_origin", self.origin );
	self PlayerLinkToDelta( cam, undefined, 1, 180, 180, 90, 90 );
	self endon( "death" );
	self.qa_look_done = false;
	self thread qa_look_done_on_stop();
	for ( ;; )
	{
		if ( !isdefined( target ) )
			break;
		yaw = target GetPlayerAngles()[ 1 ];
		f = AnglesToForward( ( 0, yaw, 0 ) );
		r = AnglesToRight( ( 0, yaw, 0 ) );
		eye = target GetEye();
		cam.origin = eye - f * 110 + r * 70 + ( 0, 0, 10 );
		self SetPlayerAngles( VectorToAngles( eye - ( cam.origin + ( 0, 0, 60 ) ) ) );
		wait 0.05;
		if ( isdefined( self.qa_look_done ) && self.qa_look_done )
			break;
	}
	self Unlink();
	cam Delete();
}

// Escape from Favela (start point "solorun_chopper"): the rooftop slide carries a player to the ledge, so a partner far behind is brought to the first one when he steps onto
// it (maps\_mw2coop_favelaescape::slide_sync: 2 s after the flag, when the other one is more than 500 units away). Player 1 is put on the slide's trigger, player 2 stays.
groundpick()
{
	check( level.players.size == 2 && isdefined( level.player2 ), "two players (" + level.players.size + ")" );
	wait_intro();
	wait 4;
	p = level.player2;
	// a level that starts in a vehicle (Wolverines!: the Humvee, weapons disabled until the players get out): wait till player 2 stands on his feet
	for ( t = 0; t < 70 && ( p IsLinked() || level.player IsLinked() ); t += 0.5 )
		wait 0.5;
	check( !p IsLinked(), "player 2 is on his feet (" + t + " s)" );
	wait 4;
	level.mw2coop_no_follow = true;
	// the weapon in his hands is taken and laid on the ground: his hands are empty or hold his other weapon, as in Vlad's report
	other = p GetCurrentWeapon();
	report( "groundpick: player 2 holds " + other + "; " + weapons_text( p ) );
	p TakeWeapon( other );
	if ( p GetCurrentWeapon() == "none" )
		report( "groundpick: player 2's hands are empty" );
	wait 1;
	fwd = AnglesToForward( ( 0, p GetPlayerAngles()[ 1 ], 0 ) );
	spot = PlayerPhysicsTrace( p.origin + fwd * 45 + ( 0, 0, 30 ), p.origin + fwd * 45 - ( 0, 0, 100 ) );
	item = Spawn( "weapon_" + other, spot + ( 0, 0, 4 ) );
	wait 0.5;
	p SetPlayerAngles( VectorToAngles( item.origin - p GetEye() ) );
	wait 0.5;
	qa_shot( "gp-before" );
	presses = 0;
	for ( presses = 0; presses < 4 && !p HasWeapon( other ); presses++ )
	{
		if ( isdefined( item ) )
			p SetPlayerAngles( VectorToAngles( item.origin - p GetEye() ) );
		wait 0.2;
		SetDvar( "mw2coop_qa_pad2", "0 0 0 0 16384 0 0 0" );	// X held: Use
		wait 1.2;
		SetDvar( "mw2coop_qa_pad2", "0 0 0 0 0 0 0 0" );
		wait 0.6;
	}
	check( p HasWeapon( other ), "player 2 picked the " + other + " up with Use (" + presses + " presses; " + weapons_text( p ) + ")" );
	wait 1.5;
	check( p GetCurrentWeapon() == other, "the picked weapon is player 2's weapon in hands (" + p GetCurrentWeapon() + ")" );
	if ( !p HasWeapon( other ) && isdefined( item ) )
	{
		// control (2026-11-23: player 2 never took it on 64 and 32 bits, and no "hold to pick up" hint showed in his half): player 1, on the keyboard, takes
		// the same item from the other side. If he cannot either, the item made by Spawn() is the trouble, not player 2's input
		level.player TakeWeapon( other );
		wait 0.5;
		spot1 = PlayerPhysicsTrace( item.origin + fwd * 40 + ( 0, 0, 30 ), item.origin + fwd * 40 - ( 0, 0, 100 ) );
		level.player SetOrigin( spot1 );
		level.player SetPlayerAngles( VectorToAngles( item.origin - level.player GetEye() ) );
		wait 0.8;
		qa_shot( "gp-p1-before" );
		for ( i = 0; i < 3 && !level.player HasWeapon( other ); i++ )
		{
			press( level.player, "activate", 1200 );
			wait 1.6;
		}
		took = "could not take";
		if ( level.player HasWeapon( other ) )
			took = "took";
		report( "groundpick: control: player 1 " + took + " the " + other + " (" + weapons_text( level.player ) + ")" );
		qa_shot( "gp-p1-after" );
	}
	p SetPlayerAngles( ( 0, p GetPlayerAngles()[ 1 ], 0 ) );
	wait 0.5;
	qa_shot( "gp-after" );
	wait 3;
	qa_shot( "gp-after2" );
	report( "done" );
}

// the nearest weapon item lying in the level that player p does not have (any classname weapon_*); undefined if none
deadpick_nearest_item( p, max_dist )
{
	best = undefined;
	foreach ( e in GetEntArray() )
	{
		if ( !isdefined( e.classname ) || e.classname.size <= 7 || GetSubStr( e.classname, 0, 7 ) != "weapon_" )
			continue;
		name = GetSubStr( e.classname, 7 );
		if ( p HasWeapon( name ) || Distance( e.origin, p.origin ) > max_dist )
			continue;
		if ( !isdefined( best ) || Distance( e.origin, p.origin ) < Distance( best.origin, p.origin ) )
			best = e;
	}
	return best;
}

// one more pickup for deadpick3: the nearest weapon lying on the ground that player 2 does not have
deadpick_round( p, round )
{
	item = deadpick_nearest_item( p, 1500 );
	w = undefined;
	if ( isdefined( item ) )
		w = GetSubStr( item.classname, 7 );
	check( isdefined( item ), "round " + round + ": a weapon lies near player 2 that he does not have" );
	if ( !isdefined( item ) )
		return;
	presses0 = maps\_mw2coop::int_or_zero( level.mw2coop_weapnext_presses );
	fwd = AnglesToForward( ( 0, p GetPlayerAngles()[ 1 ], 0 ) );
	p SetOrigin( PlayerPhysicsTrace( item.origin - fwd * 40 + ( 0, 0, 30 ), item.origin - fwd * 40 - ( 0, 0, 100 ) ) );
	p SetPlayerAngles( VectorToAngles( item.origin - p GetEye() ) );
	wait 0.8;
	p thread deadpick_watch_hands( 6 );
	for ( presses = 0; presses < 3 && !p HasWeapon( w ); presses++ )
	{
		if ( isdefined( item ) )
			p SetPlayerAngles( VectorToAngles( item.origin - p GetEye() ) );
		// both ways, always: a split screen reads his pad's QA dvar and ignores player 2's press(); a network pair's client presses +activate and
		// never reads the pad dvar (mw2coop_native_split is set only by the menu's split screen, not by a QA run's settings.ini)
		SetDvar( "mw2coop_qa_pad2", "0 0 0 0 16384 0 0 0" );	// X held: Use
		press( p, "activate", 1200 );
		wait 0.9;
		SetDvar( "mw2coop_qa_pad2", "0 0 0 0 0 0 0 0" );
		wait 0.6;
	}
	check( p HasWeapon( w ), "round " + round + ": player 2 picked " + w + " up (" + presses + " presses; " + weapons_text( p ) + ")" );
	wait 2.5;
	check( p GetCurrentWeapon() == w, "round " + round + ": " + w + " is in his hands 2.5 s later (" + p GetCurrentWeapon() + ")" );
	presses1 = maps\_mw2coop::int_or_zero( level.mw2coop_weapnext_presses );
	check( presses1 == presses0, "round " + round + ": nobody pressed his weapon key during the pickup (" + ( presses1 - presses0 ) + " presses)" );
}

// every change of the weapon in his hands, frame by frame, for s seconds (a pickup that leaves the hands "none" for a moment set off sync_weapons' empty-hands rule)
deadpick_watch_hands( s )
{
	was = self GetCurrentWeapon();
	report( "deadpick: hands at " + GetTime() + ": " + was );
	for ( t = 0; t < s; t += 0.05 )
	{
		wait 0.05;
		now = self GetCurrentWeapon();
		if ( now != was )
			report( "deadpick: hands at " + GetTime() + ": " + now + " (" + weapons_text( self ) + ")" );
		was = now;
	}
}

deadpick()
{
	check( level.players.size == 2 && isdefined( level.player2 ), "two players (" + level.players.size + ")" );
	wait_intro();
	wait 4;
	p = level.player2;
	for ( t = 0; t < 70 && ( p IsLinked() || level.player IsLinked() ); t += 0.5 )
		wait 0.5;
	check( !p IsLinked(), "player 2 is on his feet (" + t + " s)" );
	wait 4;
	level.mw2coop_no_follow = true;
	// a weapon lying in the level that player 2 does not have (an ally killed by Kill() drops nothing: he goes ragdoll at once, animscripts
	// DropWeaponWrapper "too late"; 2026-11-23 Invasion has a scar_h_reflex lying near the start)
	names = strtok( "scar_h_reflex m4_grunt m4m203_reflex scar_h scar_h_acog famas_reflex tavor_reflex ak47 ak47_reflex m240 rpd ump45 mp5 aug_reflex fn2000_reflex beretta usp", " " );
	item = undefined;
	w = undefined;
	for ( waited = 0; waited < 40 && !isdefined( item ) && !isdefined( deadpick_nearest_item( p, 100000 ) ); waited++ )	// (the ally whose weapon lies there dies in the opening scene, not always at the same moment)
	{
		if ( waited )
			wait 1;
		if ( waited == 8 )	// none yet: an AI near him dies by damage (its death animation drops the weapon; Kill() drops none)
		{
			victim = undefined;
			seen = "";
			foreach ( ai in GetAIArray() )
				if ( Distance( ai.origin, p.origin ) < 4000 )
				{
					wname = "?";
					if ( isdefined( ai.weapon ) )
						wname = ai.weapon;
					seen += ai.team + ":" + wname + ":" + isdefined( ai.magic_bullet_shield ) + ":" + int( Distance( ai.origin, p.origin ) ) + " ";
				}
			report( "deadpick: AI near player 2 (team:weapon:shielded:distance): " + GetAIArray().size + " in all; " + seen );
			foreach ( ai in GetAIArray() )
				if ( IsAlive( ai ) && !isdefined( ai.magic_bullet_shield ) && isdefined( ai.weapon ) && ai.weapon != "none" && Distance( ai.origin, p.origin ) < 2500
					&& ( !isdefined( victim ) || Distance( ai.origin, p.origin ) < Distance( victim.origin, p.origin ) ) )
					victim = ai;
			if ( isdefined( victim ) )
			{
				report( "deadpick: no weapon lies: " + victim.classname + " with " + victim.weapon + " at " + int( Distance( victim.origin, p.origin ) ) + " units dies" );
				dropped = victim.weapon;
				victim DoDamage( victim.health + 1000, victim.origin );
				wait 3;
				if ( !is_in_array( names, dropped ) )
					names[ names.size ] = dropped;
			}
		}
		foreach ( name in names )
		{
			if ( p HasWeapon( name ) )
				continue;
			found = GetEntArray( "weapon_" + name, "classname" );
			foreach ( it in found )
				if ( !isdefined( item ) || Distance( it.origin, p.origin ) < Distance( item.origin, p.origin ) )
				{
					item = it;
					w = name;
				}
		}
	}
	if ( !isdefined( item ) )	// any other weapon item lying in the level (placed by the map or dropped)
	{
		item = deadpick_nearest_item( p, 100000 );
		if ( isdefined( item ) )
			w = GetSubStr( item.classname, 7 );
	}
	check( isdefined( item ), "a weapon lies in the level that player 2 does not have" );
	if ( !isdefined( item ) )
	{
		report( "done" );
		return;
	}
	report( "deadpick: player 2 holds " + p GetCurrentWeapon() + "; " + weapons_text( p ) + "; the weapon on the ground: " + w + " at " + vec_int( item.origin ) + ", " + int( Distance( item.origin, p.origin ) ) + " away" );
	wait 1;	// (it lands)
	// player 2 beside it, 40 units away on the floor, facing it
	fwd = AnglesToForward( ( 0, p GetPlayerAngles()[ 1 ], 0 ) );
	spot2 = PlayerPhysicsTrace( item.origin - fwd * 40 + ( 0, 0, 30 ), item.origin - fwd * 40 - ( 0, 0, 100 ) );
	p SetOrigin( spot2 );
	p SetPlayerAngles( VectorToAngles( item.origin - p GetEye() ) );
	wait 0.5;
	qa_shot( "dp-before" );
	p thread deadpick_watch_hands( 12 );
	presses = 0;
	for ( presses = 0; presses < 4 && !p HasWeapon( w ); presses++ )
	{
		if ( isdefined( item ) )
			p SetPlayerAngles( VectorToAngles( item.origin - p GetEye() ) );
		wait 0.2;
		// both ways, always: a split screen reads his pad's QA dvar and ignores player 2's press(); a network pair's client presses +activate and
		// never reads the pad dvar (mw2coop_native_split is set only by the menu's split screen, not by a QA run's settings.ini)
		SetDvar( "mw2coop_qa_pad2", "0 0 0 0 16384 0 0 0" );	// X held: Use
		press( p, "activate", 1200 );
		wait 0.9;
		SetDvar( "mw2coop_qa_pad2", "0 0 0 0 0 0 0 0" );
		wait 0.6;
		report( "deadpick: after press " + ( presses + 1 ) + ": player 2 holds " + p GetCurrentWeapon() + "; " + weapons_text( p ) );
	}
	check( p HasWeapon( w ), "player 2 picked the dropped " + w + " up with Use (" + presses + " presses; " + weapons_text( p ) + ")" );
	wait 1.5;
	check( p GetCurrentWeapon() == w, "the picked weapon is player 2's weapon in hands (" + p GetCurrentWeapon() + ")" );
	if ( !p HasWeapon( w ) && isdefined( item ) )
	{
		spot1 = PlayerPhysicsTrace( item.origin + fwd * 40 + ( 0, 0, 30 ), item.origin + fwd * 40 - ( 0, 0, 100 ) );
		level.player SetOrigin( spot1 );
		level.player SetPlayerAngles( VectorToAngles( item.origin - level.player GetEye() ) );
		wait 0.8;
		qa_shot( "dp-p1-before" );
		for ( i = 0; i < 3 && !level.player HasWeapon( w ); i++ )
		{
			press( level.player, "activate", 1200 );
			wait 1.6;
		}
		took = "could not take";
		if ( level.player HasWeapon( w ) )
			took = "took";
		report( "deadpick: control: player 1 " + took + " the " + w + " (" + weapons_text( level.player ) + ")" );
	}
	// deadpick3: three more pickups, each time the weapon lying nearest (the one he just dropped among them); the empty-hands rule of sync_weapons must not
	// press his weapon key in the middle of a pickup (level.mw2coop_weapnext_presses counts its presses)
	if ( isdefined( level.mw2coop_qa_repeat ) )
	{
		for ( round = 2; round <= 4; round++ )
		{
			deadpick_round( p, round );
		}
	}
	p SetPlayerAngles( ( 0, p GetPlayerAngles()[ 1 ], 0 ) );
	wait 0.5;
	qa_shot( "dp-after" );
	wait 3;
	qa_shot( "dp-after2" );
	report( "done" );
}

ping_kinds( player )
{
	text = "";
	if ( !isdefined( player.mw2coop_pings ) )
		return "none";
	foreach ( ping in player.mw2coop_pings )
	{
		huds = 0;
		if ( isdefined( ping.huds ) )
			huds = ping.huds.size;
		text = text + ping.kind + "/" + huds + " ";
	}
	if ( text == "" )
		return "none";
	return text;
}

// the Special Ops snowmobile races: the start (see the scenario list)
racestart()
{
	if ( !isdefined( level.mw2coop_qa_n ) )
		level.mw2coop_qa_n = 0;	// (not started by main())
	SetDvar( "mw2coop_qa_pad2", "0 32767 0 0 0 0 0" );
	for ( t = 0; t < 300 && !( isdefined( level.player.snowmobile ) && isdefined( level.player2 ) && isdefined( level.player2.snowmobile ) ); t++ )
		wait 0.1;
	ok = isdefined( level.player.snowmobile ) && isdefined( level.player2 ) && isdefined( level.player2.snowmobile ) && flag_exist( "race_started" );
	check( ok, "both players on their snowmobiles" );
	if ( !ok )
	{
		SetDvar( "mw2coop_qa_pad2", "" );
		return;
	}
	s1 = level.player.snowmobile;
	s2 = level.player2.snowmobile;
	o1 = s1.origin;
	o2 = s2.origin;
	max1 = 0;
	max2 = 0;
	shot = false;
	for ( n = 0; !flag( "race_started" ); n++ )
	{
		d1 = Distance( s1.origin, o1 );
		d2 = Distance( s2.origin, o2 );
		if ( d1 > max1 )
			max1 = d1;
		if ( d2 > max2 )
			max2 = d2;
		if ( n % 5 == 0 )
			report( "racestart: before GO, moved: player 1 " + int( d1 ) + ", player 2 " + int( d2 ) + " units" );
		if ( !shot && n >= 25 )
		{
			SetDvar( "mw2coop_qa_shot", level.script + "-race-countdown" );	// (qa_shot names the start point, which a Special Ops level may not have)
			shot = true;
		}
		wait 0.1;
	}
	check( max1 < 24, "player 1's snowmobile held until GO (it moved " + int( max1 ) + " units at most)" );
	check( max2 < 24, "player 2's snowmobile held until GO (it moved " + int( max2 ) + " units at most)" );
	g1 = s1.origin;
	g2 = s2.origin;
	wait 3;
	SetDvar( "mw2coop_qa_shot", level.script + "-race-go" );
	check( Distance( s1.origin, g1 ) > 100, "player 1 drives after GO (" + int( Distance( s1.origin, g1 ) ) + " units in 3 s)" );
	check( Distance( s2.origin, g2 ) > 100, "player 2 drives after GO (" + int( Distance( s2.origin, g2 ) ) + " units in 3 s)" );
	SetDvar( "mw2coop_qa_pad2", "" );
	report( "racestart: done" );
}

// the hints' texts (maps\_mw2coop main, before any wait)
hintkeys_precache()
{
	PrecacheString( &"SCRIPT_PLATFORM_AC130_HINT_ZOOM_AND_FIRE" );
	PrecacheString( &"SCRIPT_PLATFORM_MINIGUN_SPIN_HINT" );
	PrecacheString( &"SCRIPT_PLATFORM_SNOWMOBILE_DRIVE" );
	PrecacheString( &"SCRIPT_PLATFORM_SNOWMOBILE_REVERSE" );
	PrecacheString( &"SCRIPT_PLATFORM_SNOWMOBILE_ATTACK" );
	PrecacheString( &"SCRIPT_PLATFORM_ZODIAC_STEADY" );
	PrecacheString( &"SPECIAL_OPS_PRESS_TO_CANCEL" );
	PrecacheString( &"SCRIPT_HINT_THROW_C4" );
	PrecacheString( &"SCRIPT_PLATFORM_OILRIG_HINT_STEALTH_KILL" );
}

// the hints' keys (see the scenario list): a text element of each player's own, as the game's hints are (its key names are made in his half's context; iPrintLnBold
// leaves "[{...}]" as it is)
hintkeys()
{
	check( level.players.size == 2 && isdefined( level.player2 ), "two players (" + level.players.size + ")" );
	wait_intro();
	wait 3;
	level.player EnableInvulnerability();
	level.player2 EnableInvulnerability();
	names = StrTok( "AC130_ZOOM MINIGUN_SPIN SNOWMOBILE_DRIVE SNOWMOBILE_REVERSE SNOWMOBILE_ATTACK ZODIAC_STEADY SO_CANCEL THROW_C4 STEALTH_KILL", " " );
	foreach ( i, n in names )
	{
		huds = [];
		foreach ( player in level.players )
		{
			h = NewClientHudElem( player );
			h.alignX = "center";
			h.alignY = "middle";
			h.horzAlign = "center";
			h.vertAlign = "middle";
			h.x = 0;
			h.y = -50;
			h.font = "default";
			h.fontscale = 1.6;
			h.foreground = true;
			switch ( n )
			{
				case "AC130_ZOOM": h SetText( &"SCRIPT_PLATFORM_AC130_HINT_ZOOM_AND_FIRE" ); break;
				case "MINIGUN_SPIN": h SetText( &"SCRIPT_PLATFORM_MINIGUN_SPIN_HINT" ); break;
				case "SNOWMOBILE_DRIVE": h SetText( &"SCRIPT_PLATFORM_SNOWMOBILE_DRIVE" ); break;
				case "SNOWMOBILE_REVERSE": h SetText( &"SCRIPT_PLATFORM_SNOWMOBILE_REVERSE" ); break;
				case "SNOWMOBILE_ATTACK": h SetText( &"SCRIPT_PLATFORM_SNOWMOBILE_ATTACK" ); break;
				case "ZODIAC_STEADY": h SetText( &"SCRIPT_PLATFORM_ZODIAC_STEADY" ); break;
				case "SO_CANCEL": h SetText( &"SPECIAL_OPS_PRESS_TO_CANCEL" ); break;
				case "THROW_C4": h SetText( &"SCRIPT_HINT_THROW_C4" ); break;
				default: h SetText( &"SCRIPT_PLATFORM_OILRIG_HINT_STEALTH_KILL" ); break;
			}
			huds[ huds.size ] = h;
		}
		wait 0.6;
		SetDvar( "mw2coop_qa_shot", level.script + "-hint-" + i + "-" + n );
		report( "hintkeys: " + i + " " + n + " shown" );
		wait 1.2;
		foreach ( h in huds )
			h Destroy();
		wait 0.3;
	}
	report( "hintkeys: done" );
}

// a marker's voice line ("none" when it has none)
ping_voice( ping )
{
	if ( !isdefined( ping ) || !isdefined( ping.voice ) )
		return "none";
	return ping.voice;
}

// a line without its clock number ("US_2_callout_yourclock_3" -> "US_2_callout_yourclock")
ping_line_base( v )
{
	if ( !IsSubStr( v, "clock_" ) )
		return v;
	parts = StrTok( v, "_" );
	text = parts[ 0 ];
	for ( i = 1; i < parts.size - 1; i++ )
		text = text + "_" + parts[ i ];
	return text;
}

// which of the markers' lines the level loads, for voices 1 and 2 of side c
ping_lines( c )
{
	if ( c == "" )
		return "no side";
	text = "";
	foreach ( s in StrTok( "callout_yourclock_12 callout_contactclock_12 threat_mg_generic threat_infantry_exposed exposed_open threat_infantry_generic inform_incoming_grenade order_action_suppress order_move_combat", " " ) )
	{
		have = "";
		if ( SoundExists( c + "_1_" + s ) )
			have = have + "1";
		if ( SoundExists( c + "_2_" + s ) )
			have = have + "2";
		if ( have == "" )
			have = "-";
		text = text + s + " " + have + "; ";
	}
	return text;
}

// any level: the markers' voices there (the side, its lines) and one marker of each player: player 1's "going here", player 2's "enemy" on a spot ahead of him
pingvoice()
{
	check( level.players.size == 2 && isdefined( level.player2 ), "two players (" + level.players.size + ")" );
	wait_intro();
	wait 4;
	for ( t = 0; t < 20 && !isdefined( level.player.mw2coop_pings ); t++ )
		wait 0.5;
	check( isdefined( level.player.mw2coop_pings ), "the markers are set up (maps\\_mw2coop_ping::init)" );
	p1 = level.player;
	p2 = level.player2;
	p1 EnableInvulnerability();
	p2 EnableInvulnerability();
	campaign = "none";
	if ( isdefined( level.campaign ) )
		campaign = level.campaign;
	c = maps\_mw2coop_ping::voice_country();
	report( "pingvoice: " + level.script + ", campaign " + campaign + ": voices '" + c + "' (" + ping_lines( c ) + ")" );
	want = "SP_hit_alert";
	if ( c != "" && SoundExists( c + "_1_order_move_combat" ) )
		want = c + "_1_order_move_combat";
	p1 SetPlayerAngles( ( 20, p1 GetPlayerAngles()[ 1 ], 0 ) );
	wait 0.3;
	p1 maps\_mw2coop_ping::event( "open", -1 );
	wait 0.2;
	p1 maps\_mw2coop_ping::event( "sel", 2 );
	wait 0.2;
	p1 maps\_mw2coop_ping::event( "close", 2 );
	wait 0.6;
	last = ping_last( p1 );
	check( isdefined( last ) && last.kind == "go" && ping_voice( last ) == want, level.script + ": player 1's 'going here' says " + want + " (" + ping_voice( last ) + ")" );
	p2 SetPlayerAngles( ( 20, p2 GetPlayerAngles()[ 1 ], 0 ) );
	wait 0.3;
	p2 maps\_mw2coop_ping::event( "open", -1 );
	wait 0.2;
	p2 maps\_mw2coop_ping::event( "sel", 0 );
	wait 0.2;
	p2 maps\_mw2coop_ping::event( "close", 0 );
	wait 0.6;
	last = ping_last( p2 );
	want = "SP_hit_alert";
	if ( c != "" && isdefined( last ) )
	{
		clock = animscripts\battlechatter::getDirectionFacingClock( p1 GetPlayerAngles(), p1.origin, last.pos );
		foreach ( s in StrTok( "callout_yourclock_" + clock + " callout_contactclock_" + clock + " threat_infantry_exposed exposed_open threat_infantry_generic", " " ) )
			if ( SoundExists( c + "_2_" + s ) )
			{
				want = c + "_2_" + s;
				break;
			}
	}
	check( isdefined( last ) && last.kind == "enemy" && ping_line_base( ping_voice( last ) ) == ping_line_base( want ), level.script + ": player 2's 'enemy' says " + want + " (" + ping_voice( last ) + ")" );
	qa_shot( "pingvoice" );
	wait 2;
	report( "pingvoice: done" );
}

ping_last( player )
{
	if ( !isdefined( player.mw2coop_pings ) || !player.mw2coop_pings.size )
		return undefined;
	return player.mw2coop_pings[ player.mw2coop_pings.size - 1 ];
}

// turns the player to a point and taps (the script's event, as the DLL's would come)
ping_at( player, point )
{
	level.mw2coop_qa_ping_aim = GetTime();
	player SetPlayerAngles( VectorToAngles( point - player GetEye() ) );
	wait 0.25;
	player maps\_mw2coop_ping::event( "tap", -1 );
	wait 0.5;
}

// the nearest entity of the list that the player sees (its point: origin + lift), within reach
ping_seen( player, ents, lift, reach )
{
	best = undefined;
	eye = player GetEye();
	foreach ( e in ents )
	{
		if ( !isdefined( e ) || Distance( e.origin, eye ) > reach )
			continue;
		at = e.origin + ( 0, 0, lift );
		tr = BulletTrace( eye, at, false, player );
		if ( tr[ "fraction" ] < 0.97 && !( isdefined( tr[ "entity" ] ) && tr[ "entity" ] == e ) )
			continue;
		if ( !isdefined( best ) || Distance( e.origin, eye ) < Distance( best.origin, eye ) )
			best = e;
	}
	return best;
}

pingtest()
{
	check( level.players.size == 2 && isdefined( level.player2 ), "two players (" + level.players.size + ")" );
	wait_intro();
	wait 4;
	for ( t = 0; t < 20 && !isdefined( level.player.mw2coop_pings ); t++ )
		wait 0.5;
	check( isdefined( level.player.mw2coop_pings ), "the markers are set up (maps\\_mw2coop_ping::init)" );
	p1 = level.player;
	p2 = level.player2;
	level.mw2coop_no_follow = true;
	p1 EnableInvulnerability();
	p2 EnableInvulnerability();
	level thread ping_yaw_watch( p1 );
	// player 1: a place 30 degrees down in front of him
	p1 SetPlayerAngles( ( 30, p1 GetPlayerAngles()[ 1 ], 0 ) );
	wait 0.3;
	p1 maps\_mw2coop_ping::event( "tap", -1 );
	wait 0.6;
	check( p1.mw2coop_pings.size == 1 && p1.mw2coop_pings[ 0 ].kind == "place" && p1.mw2coop_pings[ 0 ].huds.size == 2, "player 1 marked a place, seen by both (" + ping_kinds( p1 ) + ")" );
	qa_shot( "ping-place" );
	// on it again a second later: it goes away (he is turned to its spot again: the start of Invasion turns him with the Humvee, and the press of the first run
	// 2026-11-23 landed 600 units away)
	wait 1;
	level.mw2coop_qa_ping_aim = GetTime();
	if ( p1.mw2coop_pings.size && isdefined( p1.mw2coop_pings[ 0 ].spot ) )
		p1 SetPlayerAngles( VectorToAngles( p1.mw2coop_pings[ 0 ].spot - p1 GetEye() ) );
	p1 maps\_mw2coop_ping::event( "tap", -1 );	// (same frame: the Humvee of Invasion's start turns him between frames)
	wait 0.6;
	check( p1.mw2coop_pings.size == 0, "player 1's press on his own marker took it away (" + ping_kinds( p1 ) + ")" );
	// twice quickly: danger
	p1 maps\_mw2coop_ping::event( "tap", -1 );
	wait 0.15;
	p1 maps\_mw2coop_ping::event( "tap", -1 );
	wait 0.6;
	check( p1.mw2coop_pings.size == 1 && p1.mw2coop_pings[ 0 ].kind == "danger", "two quick presses: danger (" + ping_kinds( p1 ) + ")" );
	qa_shot( "ping-danger" );
	// the wheel: open, "watching" (6, left), let go
	p1 SetPlayerAngles( ( 15, p1 GetPlayerAngles()[ 1 ] + 40, 0 ) );
	wait 2.5;	// (the 5 presses in 3 s guard)
	p1 maps\_mw2coop_ping::event( "open", -1 );
	wait 0.3;
	check( isdefined( p1.mw2coop_wheel ) && p1.mw2coop_wheel.boxes.size == 10 && !isdefined( p2.mw2coop_wheel ), "the wheel opened for player 1 only (ten items)" );
	p1 maps\_mw2coop_ping::event( "sel", 6 );
	wait 0.4;
	qa_shot( "ping-wheel" );
	p1 maps\_mw2coop_ping::event( "close", 6 );
	wait 0.6;
	last = ping_last( p1 );
	check( !isdefined( p1.mw2coop_wheel ) && isdefined( last ) && last.kind == "watch", "let go on 'watching': the wheel is gone and a watch marker made (" + ping_kinds( p1 ) + ")" );
	qa_shot( "ping-watch" );
	// the wheel let go in the middle: nothing
	n = p1.mw2coop_pings.size;
	p1 maps\_mw2coop_ping::event( "open", -1 );
	wait 0.2;
	p1 maps\_mw2coop_ping::event( "close", -1 );
	wait 0.4;
	check( !isdefined( p1.mw2coop_wheel ) && p1.mw2coop_pings.size == n, "let go in the middle: no marker (" + ping_kinds( p1 ) + ")" );
	// the voices: "watching" has no line; "going here" and "attacking" are player 1's (the campaign's side: Invasion's Rangers)
	c = maps\_mw2coop_ping::voice_country();
	report( "pingtest: the markers' voices: '" + c + "' (" + ping_lines( c ) + ")" );
	check( c == "US", "Invasion's markers speak with the Rangers' voices ('" + c + "')" );
	check( isdefined( last ) && ping_voice( last ) == "SP_hit_alert", "'watching' has no line: the short sound (" + ping_voice( last ) + ")" );
	wait 2.5;
	p1 maps\_mw2coop_ping::event( "open", -1 );
	wait 0.2;
	p1 maps\_mw2coop_ping::event( "sel", 2 );
	wait 0.2;
	p1 maps\_mw2coop_ping::event( "close", 2 );
	wait 0.6;
	last = ping_last( p1 );
	check( isdefined( last ) && last.kind == "go" && ping_voice( last ) == c + "_1_order_move_combat", "'going here' in player 1's voice (" + ping_kinds( p1 ) + ": " + ping_voice( last ) + ")" );
	wait 1.7;
	p1 maps\_mw2coop_ping::event( "open", -1 );
	wait 0.2;
	p1 maps\_mw2coop_ping::event( "sel", 1 );
	wait 0.2;
	p1 maps\_mw2coop_ping::event( "close", 1 );
	wait 0.6;
	last = ping_last( p1 );
	check( isdefined( last ) && last.kind == "attack" && ping_voice( last ) == c + "_1_order_action_suppress", "'attacking' in player 1's voice (" + ping_kinds( p1 ) + ": " + ping_voice( last ) + ")" );

	// targets as they come in sight (Invasion: the enemies come up the street a while after the start)
	enemy_done = false;
	weapon_done = false;
	car_done = false;
	vehicle_done = false;
	put_tried = false;
	for ( t = 0; t < 180 && !( enemy_done && weapon_done && car_done && vehicle_done ); t++ )
	{
		wait 0.5;
		if ( !car_done )
		{
			cars = array_combine( GetEntArray( "destructible_vehicle", "targetname" ), GetEntArray( "explodable_barrel", "targetname" ) );
			car = ping_seen( p1, cars, 30, 1500 );
			if ( isdefined( car ) && !isdefined( car.exploded ) )
			{
				wait 3.1;
				ping_at( p1, car.origin + ( 0, 0, 30 ) );
				last = ping_last( p1 );
				check( isdefined( last ) && IsSubStr( last.kind, "explosive" ) && isdefined( last.ent ) && last.ent == car, "player 1 marked a car or barrel that explodes (" + ping_kinds( p1 ) + ", " + car.targetname + ", " + int( Distance( car.origin, p1.origin ) ) + " units)" );
				qa_shot( "ping-explosive" );
				car_done = true;
				continue;
			}
		}
		if ( !vehicle_done )
		{
			vehicles = [];
			foreach ( v in Vehicle_GetArray() )
				if ( isdefined( v ) && isdefined( v.code_classname ) && v.code_classname == "script_vehicle" && v.classname != "script_vehicle_corpse" )
					vehicles[ vehicles.size ] = v;
			v = ping_seen( p1, vehicles, 40, 3000 );
			if ( isdefined( v ) && Distance( v.origin, p1.origin ) > 250 )
			{
				wait 3.1;
				if ( !isdefined( v ) || v.classname == "script_vehicle_corpse" )
				{
					report( "pingtest: the vehicle was destroyed before the press; another is looked for" );
					continue;	// (Invasion's Humvees blow up: the first run's pick was a wreck by the time of the press)
				}
				ping_at( p1, v.origin + ( 0, 0, 40 ) );
				last = ping_last( p1 );
				check( isdefined( last ) && ( last.kind == "vehicle" || last.kind == "enemy_vehicle" ) && isdefined( last.anchor ), "player 1 marked a vehicle (" + ping_kinds( p1 ) + ", " + v.classname + ")" );
				qa_shot( "ping-vehicle" );
				vehicle_done = true;
				continue;
			}
		}
		if ( !enemy_done )
		{
			enemy = undefined;
			eye = p2 GetEye();
			foreach ( ai in GetAIArray( "axis" ) )
				if ( IsAlive( ai ) && SightTracePassed( eye, ai GetEye(), false, p2 ) && ( !isdefined( enemy ) || Distance( ai.origin, p2.origin ) < Distance( enemy.origin, p2.origin ) ) )
					enemy = ai;
			if ( !isdefined( enemy ) && t >= 40 && !put_tried )
			{
				put_tried = true;
				enemy = ping_put_by_enemy( p2 );
				wait 0.5;
				if ( isdefined( enemy ) && !( IsAlive( enemy ) && SightTracePassed( p2 GetEye(), enemy GetEye(), false, p2 ) ) )
					enemy = undefined;
			}
			if ( isdefined( enemy ) )
			{
				ping_at( p2, enemy GetTagOrigin( "j_spine4" ) );
				last = ping_last( p2 );
				check( isdefined( last ) && last.kind == "enemy" && isdefined( last.ent ) && last.ent == enemy && isdefined( last.anchor ), "player 2 marked the enemy he looks at, the marker above him (" + ping_kinds( p2 ) + ", " + int( Distance( enemy.origin, p2.origin ) ) + " units away)" );
				if ( isdefined( last ) )
				{
					clock = animscripts\battlechatter::getDirectionFacingClock( p1 GetPlayerAngles(), p1.origin, last.pos );
					check( ping_line_base( ping_voice( last ) ) == "US_2_callout_yourclock", "player 2's enemy marker: 'contact at your <n> o'clock' in his voice, from player 1's view (" + ping_voice( last ) + "; player 1's view now: " + clock + ")" );
				}
				qa_shot( "ping-enemy" );
				enemy_done = true;
				// killed, he drops his weapon: player 2 marks it
				spot = enemy.origin;
				enemy Kill();
				wait 2;
				state = "entity gone";
				if ( isdefined( enemy ) )
					state = "IsAlive " + IsAlive( enemy ) + ", IsAI " + IsAI( enemy ) + ", class " + enemy.classname;
				check( !isdefined( last.huds ), "the enemy's marker went with him (" + state + ")" );
				weapons = [];
				foreach ( e in GetEntArray() )
					if ( isdefined( e.classname ) && e.classname.size > 7 && GetSubStr( e.classname, 0, 7 ) == "weapon_" && Distance( e.origin, spot ) < 200 )
						weapons[ weapons.size ] = e;
				w = ping_seen( p2, weapons, 4, 3000 );
				if ( isdefined( w ) )
				{
					wait 3.1;
					ping_at( p2, w.origin + ( 0, 0, 2 ) );
					last = ping_last( p2 );
					check( isdefined( last ) && last.kind == "weapon", "player 2 marked the weapon the enemy dropped (" + ping_kinds( p2 ) + ", " + w.classname + ")" );
					qa_shot( "ping-weapon" );
					weapon_done = true;
				}
				else
					report( "pingtest: the killed enemy's weapon (" + weapons.size + " near him) is not in player 2's sight: not checked" );
				weapon_done = true;
			}
		}
	}
	if ( !enemy_done )
		report( "pingtest: no enemy came in player 2's sight in 90 s: the enemy marker is not checked" );
	if ( !car_done )
		report( "pingtest: no car or barrel that explodes in player 1's sight: not checked" );
	if ( !vehicle_done )
		report( "pingtest: no vehicle in player 1's sight: not checked" );
	wait 3;
	qa_shot( "ping-later" );
	// every marker goes when its time is up (12 s at most for a place, 15 s for an item): none older than 16 s
	old = 0;
	foreach ( pl in level.players )
		foreach ( ping in pl.mw2coop_pings )
			if ( GetTime() - ping.made > 16000 )
				old++;
	check( old == 0, "no marker outlives its time (" + old + " older than 16 s; player 1: " + ping_kinds( p1 ) + ", player 2: " + ping_kinds( p2 ) + ")" );
	report( "done" );
}

// player 2 is put on the ground 260 units from the nearest living enemy, where he sees him (8 directions tried); the enemy, or undefined
ping_put_by_enemy( p2 )
{
	ais = GetAIArray( "axis" );
	report( "pingtest: no enemy in player 2's sight after 20 s; " + ais.size + " enemies in the level: player 2 is put beside the nearest he can see from there" );
	if ( !ais.size )
		return ping_spawn_enemy( p2 );
	sorted = get_array_of_closest( p2.origin, ais );
	foreach ( ai in sorted )
	{
		if ( !IsAlive( ai ) )
			continue;
		for ( a = 0; a < 360; a += 45 )
		{
			spot = ai.origin + ( cos( a ) * 260, sin( a ) * 260, 0 );
			down = BulletTrace( spot + ( 0, 0, 72 ), spot - ( 0, 0, 160 ), false, ai );
			if ( down[ "fraction" ] >= 1 )
				continue;
			ground = down[ "position" ];
			if ( !SightTracePassed( ground + ( 0, 0, 60 ), ai GetEye(), false, ai ) )
				continue;
			if ( BulletTrace( ground + ( 0, 0, 4 ), ground + ( 0, 0, 72 ), false, ai )[ "fraction" ] < 1 )
				continue;	// (no room to stand)
			p2 SetOrigin( ground );
			report( "pingtest: player 2 put " + int( Distance( ground, ai.origin ) ) + " units from an enemy (" + ai.classname + ")" );
			return ai;
		}
	}
	return undefined;
}

// an enemy from the level's own spawners, put on the ground 300 units from player 2 where he sees him (8 directions tried); he fights nobody (ignoreall)
ping_spawn_enemy( p2 )
{
	spawners = GetSpawnerTeamArray( "axis" );
	eye = p2 GetEye();
	foreach ( s in spawners )
	{
		guy = s spawn_ai( true );
		if ( !isdefined( guy ) || !IsAlive( guy ) )
			continue;
		guy.ignoreall = true;
		guy.ignoreme = true;
		for ( a = 0; a < 360; a += 45 )
		{
			spot = p2.origin + ( cos( a ) * 300, sin( a ) * 300, 0 );
			down = BulletTrace( spot + ( 0, 0, 72 ), spot - ( 0, 0, 160 ), false, guy );
			if ( down[ "fraction" ] >= 1 )
				continue;
			ground = down[ "position" ];
			if ( !SightTracePassed( eye, ground + ( 0, 0, 60 ), false, p2 ) )
				continue;
			guy ForceTeleport( ground, VectorToAngles( p2.origin - ground ) );
			report( "pingtest: an enemy spawned (" + guy.classname + ") and put 300 units from player 2" );
			return guy;
		}
		guy Delete();
		report( "pingtest: no open ground 300 units round player 2 for the spawned enemy" );
		return undefined;
	}
	report( "pingtest: the level has no enemy spawner that spawns" );
	return undefined;
}

// player 1's view yaw as his wheel opens and closes, and a turn of more than 2.5 degrees in one frame while it is closed (the DLL's wheel: a mouse move turns the view
// when the wheel is closed, and must not while it is open); the script's own turns (ping_at) are left out
ping_yaw_watch( p )
{
	was = false;
	yaw = p GetPlayerAngles()[ 1 ];
	for ( ;; )
	{
		open = isdefined( p.mw2coop_wheel );
		now = p GetPlayerAngles()[ 1 ];
		if ( open != was )
		{
			if ( open )
				report( "pingtest: yaw " + now + " as the wheel opens" );
			else
				report( "pingtest: yaw " + now + " as the wheel closes" );
			yaw = now;
		}
		else if ( open && abs( now - yaw ) > 0.5 )
		{
			report( "pingtest: yaw " + now + " while the wheel is open" );
			yaw = now;
		}
		else if ( !open && abs( now - yaw ) > 2.5 && ( !isdefined( level.mw2coop_qa_ping_aim ) || GetTime() - level.mw2coop_qa_ping_aim > 1000 ) )
			report( "pingtest: yaw " + now + " turned " + ( now - yaw ) + " in a frame (wheel closed)" );
		if ( !open )
			yaw = now;
		was = open;
		wait 0.05;
	}
}

edgeyaw()
{
	check( level.players.size == 2 && isdefined( level.player2 ), "two players (" + level.players.size + ")" );
	wait 6;
	home = level.player2.origin;
	report( "player 1 at " + vec_int( level.player.origin ) + ", player 2 at " + vec_int( home ) );
	fell = 0;
	for ( yaw = 0; yaw < 360; yaw += 45 )
	{
		level.player SetPlayerAngles( ( 0, yaw, 0 ) );
		wait 0.2;
		level.player2.mw2coop_moved_how = "-";
		moved = level.player2 maps\_mw2coop::teleport_to_partner( level.player );
		at = level.player2.origin;
		wait 1.5;
		dz = level.player.origin[ 2 ] - level.player2.origin[ 2 ];
		report( "yaw " + yaw + ": moved " + moved + " (" + level.player2.mw2coop_moved_how + ") to " + vec_int( at ) + ", 1.5 s later " + vec_int( level.player2.origin ) + ", " + int( dz ) + " below player 1" );
		if ( dz > 100 )
		{
			fell++;
			level.player2 SetOrigin( home );
			level.player2 SetVelocity( ( 0, 0, 0 ) );
			wait 0.5;
		}
	}
	check( fell == 0, "player 2 never fell off the ledge (" + fell + " of 8)" );
	// what the old fallback did when nothing beside him was free: player 2 on player 1's spot (the two push apart)
	level.player SetPlayerAngles( ( 0, 90, 0 ) );
	level.player2 SetOrigin( level.player.origin );
	wait 2;
	dz = level.player.origin[ 2 ] - level.player2.origin[ 2 ];
	report( "the old fallback (player 2 on player 1's spot, yaw 90): 2 s later player 2 at " + vec_int( level.player2.origin ) + ", " + int( dz ) + " below player 1, player 1 at " + vec_int( level.player.origin ) );
	report( "done" );
}

laserdup()
{
	check( level.players.size == 2 && isdefined( level.player2 ), "two players (" + level.players.size + ")" );
	wait 8;
	allies = get_array_of_closest( level.player.origin, GetAIArray( "allies" ) );
	lit = [];
	foreach ( ai in allies )
	{
		if ( lit.size >= 4 )
			break;
		ai LaserForceOn();
		lit[ lit.size ] = ai;
	}
	check( lit.size > 0, "allies with the laser forced on (" + lit.size + ")" );
	if ( lit.size )
	{
		// both players face the first lit ally from a few steps away, so both halves show the beams
		target = lit[ 0 ].origin;
		foreach ( i, p in level.players )
		{
			ang = VectorToAngles( target + ( 0, 0, 50 ) - p GetEye() );
			p SetPlayerAngles( ( ang[ 0 ], ang[ 1 ], 0 ) );
		}
	}
	SetDvar( "mw2coop_qa_laser_on", "1" );
	report( "lasers on at " + GetTime() + " ms" );
	wait 4;
	qa_shot( "laser-a" );
	wait 12;
	qa_shot( "laser-b" );
	wait 4;
	foreach ( ai in lit )
		if ( isalive( ai ) )
			ai LaserForceOff();
	report( "lasers off at " + GetTime() + " ms" );
	report( "done" );
}

slidesync()
{
	check( level.players.size == 2 && isdefined( level.player2 ), "two players (" + level.players.size + ")" );
	wait 8;
	trigs = [];	// (GetEntArray takes no script_flag key: the flag triggers by their class, then their flag)
	foreach ( cls in strtok( "trigger_multiple_flag_set trigger_multiple_flag_set_touching trigger_multiple", " " ) )
	{
		ents = GetEntArray( cls, "classname" );
		if ( !isdefined( ents ) )
			continue;
		foreach ( e in ents )
			if ( isdefined( e.script_flag ) && e.script_flag == "trig_solorun_player_on_slide" )
				trigs[ trigs.size ] = e;
	}
	check( trigs.size > 0, "the slide's trigger exists (" + trigs.size + ")" );
	if ( !trigs.size )
		return;
	trig = trigs[ 0 ];
	report( "slide trigger at " + vec_int( trig.origin ) + ", ladder " + isdefined( level.chopperladder ) + ", player 1 at " + vec_int( level.player.origin ) + ", player 2 at " + vec_int( level.player2.origin ) + ", " + int( Distance( level.player.origin, level.player2.origin ) ) + " apart" );
	// player 2 well behind the slide (it carries a player toward the ladder): the side of the trigger away from the ladder, 700 units, on whatever is below
	if ( isdefined( level.chopperladder ) )
	{
		away = VectorNormalize( ( trig.origin[ 0 ] - level.chopperladder.origin[ 0 ], trig.origin[ 1 ] - level.chopperladder.origin[ 1 ], 0 ) );
		back = trig.origin + away * 700;
		level.player2 SetOrigin( PlayerPhysicsTrace( back + ( 0, 0, 600 ), back - ( 0, 0, 800 ) ) );
		report( "ladder at " + vec_int( level.chopperladder.origin ) + ", player 2 put behind the slide at " + vec_int( level.player2.origin ) );
		wait 1;
	}
	spot = PlayerPhysicsTrace( trig.origin + ( 0, 0, 40 ), trig.origin - ( 0, 0, 400 ) );
	level.player SetOrigin( spot );
	t = 0;
	while ( !flag( "trig_solorun_player_on_slide" ) && t < 5 )
	{
		wait 0.1;
		t += 0.1;
	}
	check( flag( "trig_solorun_player_on_slide" ), "player 1 on the slide sets its flag (" + t + " s)" );
	report( "player 1 on the slide at " + vec_int( level.player.origin ) + ", player 2 " + int( Distance( level.player.origin, level.player2.origin ) ) + " away" );
	qa_shot( "slide-a" );
	t = 0;
	while ( Distance( level.player.origin, level.player2.origin ) > 300 && t < 9 )
	{
		wait 0.25;
		t += 0.25;
	}
	d = Distance( level.player.origin, level.player2.origin );
	check( d <= 300, "player 2 was brought to player 1 (" + int( d ) + " apart after " + t + " s)" );
	qa_shot( "slide-b" );
	wait 3;
	qa_shot( "slide-c" );
	report( "after 3 s: player 1 at " + vec_int( level.player.origin ) + " alive " + isalive( level.player ) + ", player 2 at " + vec_int( level.player2.origin ) + " alive " + isalive( level.player2 ) );
	report( "done" );
}

// Escape from Favela (start point "solorun_chopper": the level's code calls that part "chopperjump", but a start
// named so does not exist and the level then starts at its beginning, where no chopper comes: 2026-11-17): player 2 leaps for the chopper's rope ladder (real keys:
// forward, then jump, facing the ladder); player 1 catches the rope too; both are on it when the
// chopper flies off.
chopperjump()
{
	check( level.players.size == 2 && isdefined( level.player2 ), "two players (" + level.players.size + ")" );
	wait 8;
	vol = GetEnt( "trig_player_chopperjump", "script_noteworthy" );
	check( isdefined( vol ), "the jump volume exists" );
	t = 0;
	while ( !isdefined( level.chopperladder ) && t < 30 )
	{
		wait 0.5;
		t += 0.5;
	}
	check( isdefined( level.chopperladder ), "the ladder exists (" + t + " s)" );
	if ( !isdefined( vol ) || !isdefined( level.chopperladder ) )
		return;
	report( "volume " + vol.origin + ", ladder " + level.chopperladder.origin + ", player 1 at " + level.player.origin + ", player 2 at " + level.player2.origin );
	// a run-up of 130 units to the ledge, facing the ladder, then jumps while running (the first
	// attempt pressed jump 0.7 s after forward: he had walked off the ledge by then)
	look = level.chopperladder.origin + ( 0, 0, -85 );
	toward = VectorNormalize( ( look[ 0 ] - vol.origin[ 0 ], look[ 1 ] - vol.origin[ 1 ], 0 ) );
	start = vol.origin - toward * 130;
	spot = PlayerPhysicsTrace( start + ( 0, 0, 40 ), start - ( 0, 0, 400 ) );
	level.player2 SetOrigin( spot );
	wait 0.5;
	level.player2 SetPlayerAngles( VectorToAngles( look - level.player2 GetEye() ) );
	wait 0.5;
	report( "player 2 starts at " + level.player2.origin + " and the volume is touched: " + level.player2 IsTouching( vol ) );
	press( level.player2, "forward", 3000 );
	wait 0.5;
	for ( n = 0; n < 12 && !flag( "chopperjump_player_jump" ); n++ )
	{
		press( level.player2, "gostand", 150 );
		wait 0.1;
		report( "run " + n + ": player 2 at " + level.player2.origin + " speed " + int( Length( level.player2 GetVelocity() ) ) + " touches " + level.player2 IsTouching( vol ) );
	}
	wait 3;
	check( flag( "chopperjump_player_jump" ), "the jump was caught" );
	// frames of both screens (host = player 1, client = player 2): on the rope, climbing, flying away
	qa_shot( "rope-a" );
	report( "rope: player 1 at " + vec_int( level.player.origin ) + ", player 2 at " + vec_int( level.player2.origin ) + ", " + int( Distance( level.player.origin, level.player2.origin ) ) + " apart" );
	qa_outside( "rope-outside" );
	wait 1.0;
	qa_shot( "rope-b" );
	wait 2.5;
	qa_shot( "rope-c" );
	check( level.player2 IsLinked(), "player 2 is on the rope" );
	check( level.player IsLinked(), "player 1 caught the rope too" );
	t = 0;
	while ( !flag( "solorun_player_boarded_chopper" ) && t < 20 )
	{
		wait 0.5;
		t += 0.5;
	}
	check( flag( "solorun_player_boarded_chopper" ), "the chopper took them" );
	wait 10;
	check( level.player2 IsLinked() && level.player IsLinked(), "both still on the rope while the chopper flies" );
	report( "done" );
}

// Any scripted scene: nobody touches anything. Every 5 s both screens are saved (host = player 1,
// client = player 2) and each player's state is logged: linked, stance, position, weapon.
scenewatch()
{
	check( level.players.size == 2 && isdefined( level.player2 ), "two players (" + level.players.size + ")" );
	for ( n = 0; n < 9; n++ )
	{
		wait 4.4;
		qa_shot( "scene" + n );
		foreach ( player in level.players )
			report( "t" + n + " " + player_name( player ) + ": at " + vec_int( player.origin ) + ", linked " + player IsLinked() + ", " + player GetStance() + ", weapon " + player GetCurrentWeapon() + ", alive " + isalive( player ) );
	}
	report( "done" );
}

// The Gulag's evacuation (start "evac"): both players lie buried, get free, clip on the Pavelow's rope
// by their own use and are pulled away together. Frames of both screens at each step.
gulagevac()
{
	check( level.players.size == 2 && isdefined( level.player2 ), "two players (" + level.players.size + ")" );
	wait 4;
	qa_shot( "buried" );
	check( level.player IsLinked() && level.player2 IsLinked(), "both are buried in the scene (" + level.player IsLinked() + " " + level.player2 IsLinked() + ")" );
	t = 0;
	while ( level.player IsLinked() && t < 40 )
	{
		wait 0.5;
		t += 0.5;
	}
	wait 1;
	check( !level.player IsLinked() && !level.player2 IsLinked(), "both are free (" + t + " s)" );
	trigger = getEntWithFlag( "player_uses_rig" );
	check( isdefined( trigger ), "the rope's trigger exists" );
	qa_shot( "free" );
	trigger notify( "trigger", level.player );
	wait 1.5;
	check( level.player IsLinked() && !level.player2 IsLinked(), "player 1 clipped on, player 2 not yet" );
	check( !flag( "player_gets_pulled" ), "nobody is pulled away yet" );
	trigger notify( "trigger", level.player2 );
	wait 1.5;
	check( level.player2 IsLinked(), "player 2 clipped on too" );
	qa_shot( "clipped" );
	report( "evac: player 1 at " + vec_int( level.player.origin ) + ", player 2 at " + vec_int( level.player2.origin ) + ", " + int( Distance( level.player.origin, level.player2.origin ) ) + " apart" );
	qa_outside( "outside" );
	t = 0;
	while ( !flag( "player_gets_pulled" ) && t < 40 )
	{
		wait 0.5;
		t += 0.5;
	}
	check( flag( "player_gets_pulled" ), "Soap pulls (" + t + " s)" );
	wait 1;
	qa_shot( "pulled" );
	wait 3;
	qa_shot( "flying" );
	check( level.player IsLinked() && level.player2 IsLinked(), "both are on the rope while it flies" );
	report( "done" );
}

nearest_seat( bird, org )
{
	best = "none";
	d = 100000;
	for ( n = 1; n <= 4; n++ )
	{
		dd = Distance( org, bird GetTagOrigin( "tag_guy" + n ) );
		if ( dd < d )
		{
			d = dd;
			best = "tag_guy" + n + " (" + int( dd ) + ")";
		}
	}
	return best;
}

// Oil Rig's escape (start "escape"): the little bird that takes the players off the roof. Sets the
// flags the walk would, waits for the landing, and logs the seats (tags) and the riders.
oilbird()
{
	check( level.players.size == 2 && isdefined( level.player2 ), "two players (" + level.players.size + ")" );
	wait 3;
	t = 0;
	while ( !flag( "escape_littlebird_landed" ) && t < 120 )
	{
		// the walk's flags, in order, as the level reaches the waits for them
		foreach ( name in strtok( "barracks_cleared player_headed_out_of_barracks player_approaching_derrick_building_exit player_approaching_helipad", " " ) )
			if ( isdefined( level.flag[ name ] ) && !flag( name ) && ( name != "player_headed_out_of_barracks" || flag( "littlebird_escape_spawned" ) ) )
				flag_set( name );
		wait 1;
		t += 1;
	}
	check( flag( "escape_littlebird_landed" ), "the little bird landed (" + t + " s)" );
	wait 4;
	bird = level.littlebird_escape;
	check( isdefined( bird ), "the bird exists" );
	if ( !isdefined( bird ) )
		return;
	report( "bird at " + vec_int( bird.origin ) + " angles " + bird.angles );
	for ( n = 1; n <= 4; n++ )
		report( "tag_guy" + n + " at " + vec_int( bird GetTagOrigin( "tag_guy" + n ) ) );
	t = 0;
	while ( ( !isdefined( bird.riders ) || bird.riders.size < 8 ) && t < 40 )
	{
		wait 0.5;
		t += 0.5;
	}
	report( "riders loaded: " + bird.riders.size + " (" + t + " s)" );
	foreach ( ai in GetAIArray( "allies" ) )
		if ( isdefined( ai.script_startingposition ) )
			report( "ally pos " + ai.script_startingposition + " at " + vec_int( ai.origin ) + " nearest seat " + nearest_seat( bird, ai.origin ) + " name " + ai.classname );
	report( "players " + vec_int( level.player.origin ) + " " + vec_int( level.player2.origin ) );
	qa_shot( "bird" );
	// both walk up to the seat trigger: the bird takes them
	spot = bird GetTagOrigin( "tag_guy3" ) + ( 0, 0, 10 );
	level.player SetOrigin( spot );
	wait 0.5;
	level.player2 SetOrigin( spot + ( 20, 0, 0 ) );
	wait 4;
	check( level.player IsLinked() && level.player2 IsLinked(), "both are seated on the bird (" + level.player IsLinked() + " " + level.player2 IsLinked() + ")" );
	t = 0;
	while ( !flag( "player_on_board_littlebird" ) && t < 20 )
	{
		wait 0.5;
		t += 0.5;
	}
	check( flag( "player_on_board_littlebird" ), "both are on board, the bird may go (" + t + " s)" );
	qa_shot( "seated" );
	report( "seats: player 1 at " + vec_int( level.player.origin ) + ", player 2 at " + vec_int( level.player2.origin ) + ", " + int( Distance( level.player.origin, level.player2.origin ) ) + " apart" );
	wait 8;
	qa_shot( "flying" );
	check( level.player IsLinked() && level.player2 IsLinked(), "both are still seated in flight" );
	report( "done" );
}

// Wolverines!' end (start "convoy"): the convoy arrives, player 1 walks to its parked Humvee and is
// seated on tag_guy1, player 2 on another free seat of the same Humvee; frames of both screens.
invconvoy()
{
	check( level.players.size == 2 && isdefined( level.player2 ), "two players (" + level.players.size + ")" );
	t = 0;
	while ( !flag( "convoy_in_position" ) && t < 120 )
	{
		wait 0.5;
		t += 0.5;
	}
	check( flag( "convoy_in_position" ), "the convoy arrived (" + t + " s)" );
	flag_set( "player_at_convoy" );
	humvee = level.convoy;
	check( isdefined( humvee ), "the player's Humvee exists" );
	if ( !isdefined( humvee ) )
		return;
	t = 0;
	while ( humvee.veh_speed != 0 && t < 40 )
	{
		wait 0.5;
		t += 0.5;
	}
	report( "the Humvee stopped (" + t + " s) at " + vec_int( humvee.origin ) );
	level.player SetOrigin( humvee GetTagOrigin( "tag_guy1" ) + ( 0, 0, 10 ) );
	wait 3;
	check( level.player IsLinked(), "player 1 is seated" );
	check( level.player2 IsLinked(), "player 2 is seated too" );
	report( "player 1 at " + vec_int( level.player.origin ) + ", player 2 at " + vec_int( level.player2.origin ) + ", " + int( Distance( level.player.origin, level.player2.origin ) ) + " apart" );
	qa_shot( "seated" );
	wait 4;
	qa_shot( "later" );
	report( "done" );
}

// Escape from Favela (start "roofrun_bigjump"): player 2 runs up and jumps over the gap; the level plays
// the animated jump for player 1 (his rig) and player 2 plays his own beside it, then both wake up.
bigjump()
{
	check( level.players.size == 2 && isdefined( level.player2 ), "two players (" + level.players.size + ")" );
	wait 8;
	vol = GetEnt( "trig_roofrun_player_bigjump_start", "targetname" );
	edge = GetStruct( "struct_player_bigjump_edge_reference", "targetname" );
	check( isdefined( vol ) && isdefined( edge ), "the jump trigger and the edge exist" );
	if ( !isdefined( vol ) || !isdefined( edge ) )
		return;
	forward = AnglesToForward( edge.angles );
	report( "edge " + vec_int( edge.origin ) + " angles " + edge.angles + ", player 1 at " + vec_int( level.player.origin ) + ", player 2 at " + vec_int( level.player2.origin ) );
	start = edge.origin - forward * 140;
	spot = PlayerPhysicsTrace( start + ( 0, 0, 40 ), start - ( 0, 0, 400 ) );
	level.player2 SetOrigin( spot );
	wait 0.5;
	level.player2 SetPlayerAngles( ( 0, edge.angles[ 1 ], 0 ) );
	wait 0.5;
	report( "player 2 starts at " + vec_int( level.player2.origin ) );
	press( level.player2, "forward", 3000 );
	wait 0.5;
	for ( n = 0; n < 14 && !flag( "roofrun_player_bigjump_start" ); n++ )
	{
		press( level.player2, "gostand", 150 );
		wait 0.1;
		report( "run " + n + ": player 2 at " + vec_int( level.player2.origin ) + " speed " + int( Length( level.player2 GetVelocity() ) ) + " touches " + level.player2 IsTouching( vol ) );
	}
	check( flag( "roofrun_player_bigjump_start" ), "the jump was caught" );
	wait 0.7;
	check( level.player IsLinked() && level.player2 IsLinked(), "both are in their jump rigs (" + level.player IsLinked() + " " + level.player2 IsLinked() + ")" );
	report( "player 1 at " + vec_int( level.player.origin ) + ", player 2 at " + vec_int( level.player2.origin ) + ", " + int( Distance( level.player.origin, level.player2.origin ) ) + " apart" );
	qa_shot( "jump-a" );
	wait 0.6;
	qa_shot( "jump-b" );
	t = 0;
	while ( !flag( "player_recovery_blackscreen" ) && t < 40 )
	{
		wait 0.5;
		t += 0.5;
	}
	check( flag( "player_recovery_blackscreen" ), "the black screen came (" + t + " s)" );
	wait 8;
	qa_shot( "wake-a" );
	t = 0;
	while ( !flag( "player_recovery_done" ) && t < 60 )
	{
		wait 0.5;
		t += 0.5;
	}
	check( flag( "player_recovery_done" ), "the wake-up is over (" + t + " s)" );
	wait 2;
	check( !level.player IsLinked() && !level.player2 IsLinked(), "both are free (" + level.player IsLinked() + " " + level.player2 IsLinked() + ")" );
	report( "after: player 1 at " + vec_int( level.player.origin ) + ", player 2 at " + vec_int( level.player2.origin ) + ", " + int( Distance( level.player.origin, level.player2.origin ) ) + " apart" );
	qa_shot( "up" );
	report( "done" );
}

// Cliffhanger, start "snowmobile": how each player's snowmobile looks to the other (frames of both
// machines; the host's is player 1's, looking at player 2's snowmobile beside him).
snowview()
{
	check( level.players.size == 2 && isdefined( level.player2 ), "two players (" + level.players.size + ")" );
	if ( level.start_point == "icepick" )
	{
		// player 2 rides, player 1 walks up and looks at his snowmobile; with the dvar mw2coop_qa_snowrider 1 the other way round (player 2 looks at
		// player 1's rider: the 32-bit split screen check of 2026-11-23, a rider seen from the other player's half)
		rider = level.player2;
		viewer = level.player;
		if ( GetDvar( "mw2coop_qa_snowrider" ) == "1" )
		{
			rider = level.player;
			viewer = level.player2;
		}
		t = 0;
		while ( !flag( "player_snowmobile_available" ) && t < 90 )
		{
			wait 0.5;
			t += 0.5;
		}
		wait 1;
		rider player_mount_vehicle( level.player_snowmobile );
		wait 5;
		v = rider.vehicle;
		sp = GetEnt( "playermobile_spawner", "targetname" );
		if ( isdefined( sp ) )
			report( "spawner " + sp.classname + " vehicletype " + sp.vehicletype );
		report( "vehicle type " + v.vehicletype + ", riders " + isdefined( v.riders ) + ", seats " + v maps\_mw2coop::seats_text() );
		report( player_name( rider ) + " rides model " + v.model + ", first person " + isdefined( v.firstPerson ) + ", copy " + isdefined( v.mw2coop_copy ) );
		// four fixed views around the snowmobile, 130 units away, standing on the snow
		report( "rider body " + isdefined( v.mw2coop_rider ) );
		if ( isdefined( v.mw2coop_rider ) )
			report( "rider at " + v.mw2coop_rider.origin + " tag_driver " + v GetTagOrigin( "tag_driver" ) + " model " + v.mw2coop_rider.model );
		if ( isdefined( v.mw2coop_copy ) )
		{
			d = v.mw2coop_copy GetTagOrigin( "tag_driver" ) - v GetTagOrigin( "tag_driver" );
			report( "tag_driver of the third-person copy minus the vehicle's: " + d + ", copy origin minus vehicle " + ( v.mw2coop_copy.origin - v.origin ) );
		}
		if ( isdefined( v.mw2coop_rider ) )
		{
			r = v.mw2coop_rider;
			center = v GetTagOrigin( "tag_driver" ) + ( 0, 0, 45 );
			// the grips: the first-person hands attached to the snowmobile hold them
			// in a thread of its own: should the vehicle not answer for the hands' bones, only it stops
			v thread report_grips();
			wait 1;
			variant = "seated";
			{
				wait 0.5;
				report( "pose " + variant + ": " + rider_bones( r, v ) );
				// right side and front-left, 110 units away, on the snow
				for ( n = 0; n < 4; n++ )
				{
					dir = AnglesToForward( ( 0, v.angles[ 1 ] - 90 + n * 90, 0 ) );
					spot = center + dir * 110;
					spot = PlayerPhysicsTrace( spot + ( 0, 0, 60 ), spot - ( 0, 0, 300 ) );
					viewer SetOrigin( spot );
					viewer SetPlayerAngles( VectorToAngles( center - viewer GetEye() ) );
					wait 0.8;
					qa_shot( "rider-pose" + variant + "-" + n );
				}
			}
		}
		// Soap keeps his own look (the rider is a copy of his model)
		if ( isdefined( level.price ) )
		{
			report( "soap wears " + level.price.model );
			spot = level.price.origin + AnglesToForward( level.price.angles ) * 90;
			spot = PlayerPhysicsTrace( spot + ( 0, 0, 60 ), spot - ( 0, 0, 300 ) );
			level.player SetOrigin( spot );
			level.player SetPlayerAngles( VectorToAngles( level.price.origin + ( 0, 0, 40 ) - level.player GetEye() ) );
			wait 0.8;
			qa_shot( "soap" );
		}
		report( "done" );
		return;
	}
	wait 6;
	foreach ( player in level.players )
	{
		v = player.vehicle;
		if ( isdefined( v ) )
			report( player_name( player ) + " rides model " + v.model + ", first person " + isdefined( v.firstPerson ) + ", hidden player body " + isdefined( player.mw2coop_x ) );
	}
	d = level.player2.vehicle.origin - level.player.vehicle.origin;
	level.player SetPlayerAngles( VectorToAngles( d ) + ( 10, 0, 0 ) );
	wait 0.5;
	qa_shot( "snow-p1-sees-p2" );
	d = level.player.vehicle.origin - level.player2.vehicle.origin;
	level.player2 SetPlayerAngles( VectorToAngles( d ) + ( 10, 0, 0 ) );
	wait 0.5;
	qa_shot( "snow-p2-sees-p1" );
	report( "done" );
}

// Cliffhanger, start point "icepick": after Soap MacTavish's icepick fight (level.price in the scripts) two snowmobiles wait; player 2
// takes the one the level made for player 1, player 1 takes the other, and the level goes on
// with player 1's as level.player_snowmobile.
snowpick()
{
	check( level.players.size == 2 && isdefined( level.player2 ), "two players (" + level.players.size + ")" );
	t = 0;
	while ( !flag( "player_snowmobile_available" ) && t < 90 )
	{
		wait 0.5;
		t += 0.5;
	}
	check( flag( "player_snowmobile_available" ), "snowmobiles available (" + t + " s)" );
	wait 1;
	check( isdefined( level.mw2coop_partner_snowmobile ), "second snowmobile spawned" );
	level.player2 player_mount_vehicle( level.player_snowmobile );
	wait 1;
	level.player player_mount_vehicle( level.mw2coop_partner_snowmobile );
	t = 0;
	while ( !flag( "player_rides_snowmobile" ) && t < 10 )
	{
		wait 0.25;
		t += 0.25;
	}
	check( flag( "player_rides_snowmobile" ), "the level goes on (player_rides_snowmobile)" );
	check( isdefined( level.player.vehicle ) && level.player_snowmobile == level.player.vehicle, "level.player_snowmobile is player 1's" );
	check( isdefined( level.player2.vehicle ) && level.player2.vehicle != level.player.vehicle, "player 2 on the other snowmobile" );
	report( "done" );
}

// Spawn audit (any map, any start point): where player 2 ends up after the level start, over
// 12 s. PASS when he is alive, up, within 300 units of player 1 and not far below him, and
// either standing on ground or held by something (linked).
bias_group( player )
{
	group = player GetThreatBiasGroup();
	if ( !isdefined( group ) )
		return "none";
	return group;
}

spawn_audit()
{
	check( level.players.size == 2 && isdefined( level.player2 ), "two players (" + level.players.size + ")" );
	starts = "";
	if ( isdefined( level.start_functions ) )
		foreach ( s in level.start_functions )
			starts += s[ "name" ] + " ";
	mode = "undefined";
	if ( isdefined( level.level_mode ) )
		mode = level.level_mode;
	report( "start point " + level.start_point + ", level_mode " + mode + ", starts: " + starts );
	samples = [];
	samples[ 0 ] = 0.5;
	samples[ 1 ] = 3;
	samples[ 2 ] = 10.5;
	waited = 0;
	foreach ( at in samples )
	{
		wait at - waited;
		waited = at;
		report( "t+" + ( at + 1.5 ) + " " + spawn_text() );
	}
	p1 = level.player;
	p2 = level.player2;
	d = Distance( p1.origin, p2.origin );
	dz = p2.origin[ 2 ] - p1.origin[ 2 ];
	check( IsAlive( p2 ) && !maps\_mw2coop::is_downed( p2 ), "player 2 alive and up" );
	check( d < 300, "player 2 near player 1 (" + int( d ) + " units)" );
	check( dz > -150, "player 2 not below player 1 (dz " + int( dz ) + ")" );
	check( maps\_mw2coop::is_standing( p2 ) || p2 IsLinked(), "player 2 on ground or held" );
	kit_checks();
	report( "done" );
}

spawn_text()
{
	p1 = level.player;
	p2 = level.player2;
	return "start " + level.start_point + ", d " + int( Distance( p1.origin, p2.origin ) ) + ", dz " + int( p2.origin[ 2 ] - p1.origin[ 2 ] )
		+ ", p1 " + int( p1.origin[ 0 ] ) + "," + int( p1.origin[ 1 ] ) + "," + int( p1.origin[ 2 ] ) + " linked " + p1 IsLinked() + " standing " + maps\_mw2coop::is_standing( p1 )
		+ ", p2 linked " + p2 IsLinked() + " standing " + maps\_mw2coop::is_standing( p2 ) + " alive " + IsAlive( p2 );
}

// Takedown opening car (default start). Names no favela script (they are not in other zones):
// the bot ducks through the crouch command's notify, as NotifyOnPlayerCommand would send it.
takedown()
{
	check( level.players.size == 2 && isdefined( level.player2 ), "two players (" + level.players.size + ")" );
	wait 2;
	p1 = level.player;
	p2 = level.player2;
	check( p1 IsLinked() && p2 IsLinked(), "both in the car (linked " + p1 IsLinked() + " " + p2 IsLinked() + ")" );
	d = Distance( p1 GetEye(), p2 GetEye() );
	check( d > 20 && d < 80, "player 2 on the back seat (eyes " + int( d ) + " units apart)" );
	flag_wait( "car_getting_shot" );
	if ( GetDvar( "mw2coop_qa" ) == "takedown2" )
	{
		p1 notify( "go_crouch" );
		for ( t = 0; t < 8 && !isdefined( p2.mw2coop_shot ); t += 0.1 )
			wait 0.1;
		check( isdefined( p2.mw2coop_shot ) && !isdefined( p1.mw2coop_shot ), "Makarov hits player 2 (standing), not player 1 (ducking)" );
		report( "done" );
		return;
	}
	foreach ( player in level.players )
		player notify( "go_crouch" );
	wait 1;
	check( isdefined( p1.mw2coop_ducking ) && p1.mw2coop_ducking && isdefined( p2.mw2coop_ducking ) && p2.mw2coop_ducking, "both duck" );
	flag_wait( "start_chase" );
	check( !isdefined( p1.mw2coop_shot ) && !isdefined( p2.mw2coop_shot ), "nobody shot while ducking" );
	foreach ( player in level.players )
		player notify( "go_stand" );
	for ( t = 0; t < 10 && ( p1 IsLinked() || p2 IsLinked() ); t += 0.25 )
		wait 0.25;
	wait 0.5;
	check( !p1 IsLinked() && !p2 IsLinked(), "both out of the car" );
	d = Distance( p1.origin, p2.origin );
	check( d < 120 && abs( p1.origin[ 2 ] - p2.origin[ 2 ] ) < 30, "out next to each other (" + int( d ) + " units)" );
	check( IsAlive( p1 ) && IsAlive( p2 ), "both alive" );
	report( "done" );
}

// Takedown opening car, player 2 ducks with real input (see the list at the top). Player 1 ducks through the notify; player 2's own stance button has to reach his
// go_crouch (the engine's "n <index>" client command), or Makarov's shots hit him.
takedown3()
{
	check( level.players.size == 2 && isdefined( level.player2 ), "two players (" + level.players.size + ")" );
	wait 2;
	p1 = level.player;
	p2 = level.player2;
	check( p1 IsLinked() && p2 IsLinked(), "both in the car (linked " + p1 IsLinked() + " " + p2 IsLinked() + ")" );
	p1 notify( "go_crouch" );
	if ( GetDvar( "mw2coop_qa" ) == "takedown3s" )
	{
		SetDvar( "mw2coop_qa_pad2", "0 0 0 0 8192 0 0 0" ); // B: stance (the pad's polling reads it about every other frame: held for a while)
		wait 0.8;
		SetDvar( "mw2coop_qa_pad2", "0 0 0 0 0 0 0 0" );
	}
	else
		press( p2, "stance", 300 );
	for ( t = 0; t < 4 && !( isdefined( p2.mw2coop_ducking ) && p2.mw2coop_ducking ); t += 0.1 )
		wait 0.1;
	check( isdefined( p2.mw2coop_ducking ) && p2.mw2coop_ducking, "player 2 ducks on his own stance button" );
	flag_wait( "car_getting_shot" );
	flag_wait( "start_chase" );
	check( !isdefined( p2.mw2coop_shot ) && !isdefined( p1.mw2coop_shot ), "Makarov hit nobody" );
	report( "done" );
}

heliend_look( target )
{
	level endon( "mw2coop_qa_done" );
	for ( n = 0; ; n++ )
	{
		// the missile hit narrows his view again (partner_holds_on): widen it
		if ( n % 10 == 0 && isdefined( level.blackhawk ) )
			self PlayerLinkToDelta( level.blackhawk, "tag_playerride", 1, 170, 170, 80, 80 );
		dir = target.origin + ( 0, 0, 40 ) - self GetEye();
		self SetPlayerAngles( VectorToAngles( dir ) );
		wait 0.05;
	}
}

heliend()
{
	// dcburning, start "heliride2": the Black Hawk is hit; what does each player see of the other afterwards
	// (owner: player 2 still sees the gunner at the minigun). Frames he-N every 0.5 s.
	level.mw2coop_no_follow = true;
	level.mw2coop_no_leash = true;
	wait 5;
	// player 2 looks at player 1 all the time (wide view limits, as gulagturn)
	level.player2 Unlink();
	level.player2 PlayerLinkToDelta( level.blackhawk, "tag_playerride", 1, 170, 170, 80, 80 );
	level.player2 thread heliend_look( level.player );
	wait 0.5;
	SetDvar( "mw2coop_qa_shot", "he-pre" );
	report( "before the hit: p1 " + vec_int( level.player.origin ) + " linked " + level.player IsLinked() + ", p2 " + vec_int( level.player2.origin ) + " linked " + level.player2 IsLinked() );
	flag_set( "player_heli_22" );
	for ( i = 0; i < 16; i++ )
	{
		wait 0.5;
		SetDvar( "mw2coop_qa_shot", "he-" + i );
		report( "frame " + i + ": p1 " + vec_int( level.player.origin ) + " linked " + level.player IsLinked() + ", p2 " + vec_int( level.player2.origin ) + " linked " + level.player2 IsLinked() );
	}
	report( "done" );
}

crashdrift()
{
	// the crash site (dcburning start "crash" or dcemp): nobody touches a key; where does each player go after the fade-up?
	// (owner, 2026-11-06: the client is pushed forward constantly when the helicopter has crashed)
	level.mw2coop_no_follow = true;
	p1 = level.player;
	p2 = level.player2;
	flag_wait( "crash_fade_up" );
	wait 3;
	a1 = p1.origin;
	a2 = p2.origin;
	for ( i = 0; i < 12; i++ )
	{
		wait 2;
		m1 = p1 GetNormalizedMovement();
		m2 = p2 GetNormalizedMovement();
		report( "t+" + ( 3 + 2 * i + 2 ) + ": p1 " + vec_int( p1.origin ) + " moved " + int( Distance( p1.origin, a1 ) ) + " linked " + p1 IsLinked() + " input " + m1[ 0 ] + " " + m1[ 1 ] + ", p2 " + vec_int( p2.origin ) + " moved " + int( Distance( p2.origin, a2 ) ) + " linked " + p2 IsLinked() + " input " + m2[ 0 ] + " " + m2[ 1 ] );
	}
	check( Distance( p1.origin, a1 ) < 60, "player 1 stays where he is without input (" + int( Distance( p1.origin, a1 ) ) + " units)" );
	check( Distance( p2.origin, a2 ) < 60, "player 2 stays where he is without input (" + int( Distance( p2.origin, a2 ) ) + " units)" );
	report( "done" );
}

empinput()
{
	// dcemp, whole sequence, nobody touches a key: both players' origin, link and the movement input the server sees, every second, until both are free
	// (owner, 2026-11-06: between the white screens the player moves forward by himself and cannot go back)
	level.mw2coop_no_follow = true;
	p1 = level.player;
	p2 = level.player2;
	last1 = p1.origin;
	last2 = p2.origin;
	for ( i = 0; i < 200; i++ )
	{
		wait 1;
		m1 = p1 GetNormalizedMovement();
		m2 = p2 GetNormalizedMovement();
		stage = "-";
		if ( flag( "iss_fx" ) )
			stage = "iss";
		if ( flag( "iss_done" ) )
			stage = "emp";
		if ( flag( "emp_back_from_whiteout" ) )
			stage = "back";
		report( i + " " + stage + ": p1 " + vec_int( p1.origin ) + " step " + int( Distance( p1.origin, last1 ) ) + " linked " + p1 IsLinked() + " input " + m1[ 0 ] + " " + m1[ 1 ] + ", p2 " + vec_int( p2.origin ) + " step " + int( Distance( p2.origin, last2 ) ) + " linked " + p2 IsLinked() + " input " + m2[ 0 ] + " " + m2[ 1 ] );
		last1 = p1.origin;
		last2 = p2.origin;
		if ( stage == "back" && !p1 IsLinked() && !p2 IsLinked() )
			break;
	}
	report( "done" );
}

crashwalk_step( player, label )
{
	fwd = AnglesToForward( ( 0, player GetPlayerAngles()[ 1 ], 0 ) );
	before = player.origin;
	press( player, label, 1000 );
	trace = "";
	for ( k = 0; k < 10; k++ )
	{
		wait 0.25;
		m = player GetNormalizedMovement();
		e = player.origin - before;
		trace += " " + int( e[ 0 ] * fwd[ 0 ] + e[ 1 ] * fwd[ 1 ] ) + "/" + m[ 0 ];
	}
	report( player_name( player ) + " " + label + " every 0.25 s (moved/input):" + trace );
	wait 0.5;
	d = player.origin - before;
	along = int( d[ 0 ] * fwd[ 0 ] + d[ 1 ] * fwd[ 1 ] );
	report( player_name( player ) + " " + label + ": moved " + along + " along his facing (total " + int( Distance2D( player.origin, before ) ) + ")" );
	return along;
}

crashwalk()
{
	// dcemp, default start, pair: real key presses at the crash site (the cabin's movement grid) once the scene lets them move.
	// (owner, 2026-11-06: the player moves forward by himself and cannot go back)
	level.mw2coop_no_follow = true;
	flag_wait( "redshirt_headshot" );
	wait 4;
	foreach ( player in level.players )
	{
		idle0 = player.origin;
		idle = "";
		for ( k = 0; k < 12; k++ )
		{
			wait 0.25;
			idle += " " + int( Distance2D( player.origin, idle0 ) ) + "/" + player GetNormalizedMovement()[ 0 ];
		}
		report( player_name( player ) + " idle 3 s every 0.25 s (moved/input):" + idle );
		f = crashwalk_step( player, "forward" );
		b = crashwalk_step( player, "back" );
		b2 = crashwalk_step( player, "back" );
		check( b < 0 || b2 < 0, player_name( player ) + " can go back (forward " + f + ", back " + b + ", back again " + b2 + ")" );
	}
	report( "done" );
}

oilrigambush_flags()
{
	text = "";
	foreach ( name in strtok( "upper_room_breached upper_room_cleared obj_c4_ambush_plant_given player_has_started_planting_c4 obj_c4_ambush_plant_complete ambush_c4_triggered player_looking_at_prisoner_sequence trig_prisoner_sequence_failsafe", " " ) )
		if ( isdefined( level.flag[ name ] ) && flag( name ) )
			text += " " + name;
	return text;
}

oilrigambush()
{
	// oilrig, start "heli": the ambush room; what the hostages do and what the flags are, every 2 s (issue 9: hostages do not disappear and die when the bodies are blown up with C4)
	level.mw2coop_no_follow = true;
	for ( i = 0; i < 12; i++ )
	{
		wait 2;
		civs = GetAIArray( "neutral" );
		alive = 0;
		hidden = 0;
		foreach ( civ in civs )
			if ( IsAlive( civ ) )
				alive++;
		report( "t " + ( i * 2 + 2 ) + ": hostages " + alive + ", p1 " + vec_int( level.player.origin ) + ", p2 " + vec_int( level.player2.origin ) + ", flags:" + oilrigambush_flags() );
	}
	report( "done" );
}

framesevery()
{
	level.mw2coop_no_follow = true;
	for ( i = 0; i < 40; i++ )
	{
		wait 1;
		SetDvar( "mw2coop_qa_shot", "fe-" + i );
		report( "frame " + i + ": p1 " + vec_int( level.player.origin ) + " linked " + level.player IsLinked() + " alive " + IsAlive( level.player ) + ", p2 " + vec_int( level.player2.origin ) + " linked " + level.player2 IsLinked() );
	}
	report( "done" );
}

oilrigclimb()
{
	level.mw2coop_no_follow = true;
	level.mw2coop_no_leash = true;
	wait 4;
	park = ( 1700, 30, -922 );
	if ( GetDvar( "mw2coop_qa" ) == "oilrigclimbb" || GetDvar( "mw2coop_qa" ) == "oilrigclimbbp" )
		park = ( 1629, 118, -922 );
	for ( who = 1; who <= 2; who++ )
	{
		p = level.player;
		o = level.player2;
		pad = "mw2coop_qa_pad1";
		if ( who == 2 )
		{
			p = level.player2;
			o = level.player;
			pad = "mw2coop_qa_pad2";
		}
		o SetOrigin( park );
		p SetOrigin( ( 1629, 175, -1006 ) );
		p SetPlayerAngles( ( 0, 270, 0 ) );
		wait 1.5;
		report( "player " + who + " at the foot: " + p.origin + " ladder flag " + flag( "player_on_ladder" ) );
		net = ( GetDvar( "mw2coop_qa" ) == "oilrigclimbp" || GetDvar( "mw2coop_qa" ) == "oilrigclimbbp" );
		if ( net )
			press( p, "forward", 8000 );
		else
			SetDvar( pad, "0 32767 0 0 0 0 0 0" );
		for ( i = 0; i < 16; i++ )
		{
			wait 0.5;
			report( "player " + who + " t " + ( ( i + 1 ) * 0.5 ) + ": " + p.origin + " flag " + flag( "player_on_ladder" ) );
			if ( i == 4 || i == 12 )
				SetDvar( "mw2coop_qa_shot", "lc" + who + "-" + i );
		}
		SetDvar( pad, "0 0 0 0 0 0 0 0" );
		wait 2;
	}
	report( "done" );
}

laptop2()
{
	level.mw2coop_no_follow = true;
	t = 0;
	while ( !level.player2 HasWeapon( "remote_missile_detonator" ) && t < 90 )
	{
		wait 1;
		t++;
	}
	report( "player 2 has the laptop: " + level.player2 HasWeapon( "remote_missile_detonator" ) + " after " + t + " s, p1 has it: " + level.player HasWeapon( "remote_missile_detonator" ) + ", p2 weapon " + level.player2 GetCurrentWeapon() + ", slot thread " + isdefined( level.player2.remotemissile_actionslot ) );
	wait 8;
	SetDvar( "mw2coop_qa_pad2", "0 0 0 0 8 0 0 0" );
	wait 0.4;
	SetDvar( "mw2coop_qa_pad2", "0 0 0 0 0 0 0 0" );
	wait 3;
	report( "after d-pad right: p2 weapon " + level.player2 GetCurrentWeapon() );
	SetDvar( "mw2coop_qa_pad2", "0 0 0 0 8 0 0 0" );
	wait 0.4;
	SetDvar( "mw2coop_qa_pad2", "0 0 0 0 0 0 0 0" );
	wait 3;
	report( "after d-pad right again: p2 weapon " + level.player2 GetCurrentWeapon() );
	SetDvar( "mw2coop_qa_pad2", "0 0 0 0 32768 0 0 0" );
	wait 0.4;
	SetDvar( "mw2coop_qa_pad2", "0 0 0 0 0 0 0 0" );
	wait 3;
	report( "after Y: p2 weapon " + level.player2 GetCurrentWeapon() );
	report( "done" );
}

issshots()
{
	level.mw2coop_no_follow = true;
	while ( !flag( "iss_fx" ) )
		wait 0.5;
	report( "iss started" );
	for ( i = 0; i < 18; i++ )
	{
		stage = "-";
		if ( flag( "iss_fx" ) )
			stage = "iss";
		if ( flag( "iss_done" ) )
			stage = "emp";
		if ( flag( "emp_back_from_whiteout" ) )
			stage = "back";
		qa_shot( "iss" + i + "-" + stage );
		report( i + " " + stage + ": p1 " + vec_int( level.player.origin ) + " p2 " + vec_int( level.player2.origin ) + " hide_names " + GetDvar( "mw2coop_hide_names" ) );
		wait 2.4;
	}
	report( "done" );
}

laptop2pair()
{
	level.mw2coop_no_follow = true;
	t = 0;
	while ( !level.player2 HasWeapon( "remote_missile_detonator" ) && t < 60 )
	{
		wait 1;
		t++;
	}
	report( "player 2 has the laptop: " + level.player2 HasWeapon( "remote_missile_detonator" ) + " after " + t + " s, weapon " + level.player2 GetCurrentWeapon() );
	wait 6;
	level.player2 SwitchToWeapon( "remote_missile_detonator" );
	wait 5;
	report( "raised: p2 weapon " + level.player2 GetCurrentWeapon() + ", using_uav " + level.player2.using_uav + ", controlling " + isdefined( level.player2.is_controlling_UAV ) );
	report( "last_weapon " + level.player2.last_weapon + ", primaries " + level.player2 GetWeaponsListPrimaries().size );
	press( level.player2, "stance", 300 );
	for ( k = 0; k < 8; k++ )
	{
		wait 1;
		report( "after his stance key +" + k + " s: p2 weapon " + level.player2 GetCurrentWeapon() + ", using_uav " + level.player2.using_uav + ", controlling " + isdefined( level.player2.is_controlling_UAV ) + ", last_weapon " + level.player2.last_weapon );
	}
	report( "done" );
}

javpair()
{
	level.mw2coop_no_follow = true;
	wait 12;
	qa_shot( "jv-0" );
	report( "before: p2 weapon " + level.player2 GetCurrentWeapon() + ", p2 at " + vec_int( level.player2.origin ) );
	items = GetEntArray( "weapon_javelin_dcburn", "classname" );
	report( "Javelin pickups: " + items.size );
	if ( items.size == 0 )
	{
		report( "no Javelin pickup in this level start" );
		report( "done" );
		return;
	}
	report( "item at " + vec_int( items[ 0 ].origin ) );
	level.player2 SetOrigin( items[ 0 ].origin + ( 0, 0, 8 ) );
	wait 1;
	press( level.player2, "activate", 700 );
	for ( k = 1; k <= 6; k++ )
	{
		wait 1.5;
		qa_shot( "jv-" + k );
		report( "after " + k + ": p2 weapon " + level.player2 GetCurrentWeapon() + ", weapons " + level.player2 GetWeaponsListAll().size + ", at " + vec_int( level.player2.origin ) );
	}
	report( "done" );
}

oilrigwhere()
{
	level.mw2coop_no_follow = true;
	wait 8;
	report( "start " + GetDvar( "start" ) + ": p1 " + level.player.origin + " angles " + level.player GetPlayerAngles() + ", p2 " + level.player2.origin );
	SetDvar( "mw2coop_qa_shot", "ow-" + GetDvar( "start" ) );
	wait 1;
	report( "done" );
}

oilrigladder()
{
	level.mw2coop_no_follow = true;
	wait 4;
	report( "p1 at " + level.player.origin + " p2 at " + level.player2.origin );
	nodes = getnodearray( "friendlyStartHeli", "targetname" );
	foreach ( n in nodes )
		report( "node friendlyStartHeli " + n.script_noteworthy + " at " + n.origin );
	n = getnode( "node_guard_scaffolding", "targetname" );
	if ( isdefined( n ) )
		report( "node_guard_scaffolding at " + n.origin );
	t = getent( "ambush_area", "targetname" );
	if ( isdefined( t ) )
		report( "ambush_area trigger at " + t.origin );
	report( "flag player_in_ambush_position " + flag( "player_in_ambush_position" ) + ", player_on_ladder " + flag( "player_on_ladder" ) + ", obj_c4_ambush_plant_complete " + flag( "obj_c4_ambush_plant_complete" ) );
	report( "done" );
}

hitdown()
{
	level.mw2coop_no_follow = true;
	wait 6;
	level.player2 DoDamage( level.player2.health + 500, level.player2.origin );
	wait 3;
	report( "player 2 down " + level.player2 ent_flag( "coop_downed" ) + ", p1 hits him" );
	for ( i = 0; i < 40; i++ )
	{
		if ( i >= 6 && i < 30 )
			for ( k = 0; k < 5; k++ )
			{
				level.player2 DoDamage( 14, level.player.origin, level.player );
				wait 0.1;
			}
		else
			wait 0.5;
		SetDvar( "mw2coop_qa_shot", "hd-" + i );
		report( "hit " + i + ": p2 hp " + level.player2.health + " down " + level.player2 ent_flag( "coop_downed" ) + " yaw " + int( level.player2 GetPlayerAngles()[ 1 ] ) + " pitch " + int( level.player2 GetPlayerAngles()[ 0 ] ) + " roll " + int( level.player2 GetPlayerAngles()[ 2 ] ) );
	}
	report( "done" );
}

downnone()
{
	level.mw2coop_no_follow = true;
	wait 6;
	level.player2 TakeAllWeapons();
	wait 1;
	report( "player 2 has weapon " + level.player2 GetCurrentWeapon() + " and goes down" );
	level.player2 DoDamage( level.player2.health + 500, level.player2.origin );
	for ( i = 0; i < 32; i++ )
	{
		SetDvar( "mw2coop_qa_shot", "dn-" + i );
		report( "none " + i + ": p2 hp " + level.player2.health + " down " + level.player2 ent_flag( "coop_downed" ) + " weapon " + level.player2 GetCurrentWeapon() + " stance " + level.player2 GetStance() + " yaw " + int( level.player2 GetPlayerAngles()[ 1 ] ) + " pitch " + int( level.player2 GetPlayerAngles()[ 0 ] ) );
		wait 1;
	}
	report( "done" );
}

cookfrag()
{
	level.mw2coop_no_follow = true;
	wait 6;
	report( "player 2 holds frag, hp " + level.player2.health + " weapon " + level.player2 GetCurrentWeapon() + " frags " + level.player2 GetWeaponAmmoClip( "fraggrenade" ) );
	SetDvar( "mw2coop_qa_pad2", "0 0 0 0 512 0 0 0" );
	for ( i = 0; i < 32; i++ )
	{
		if ( i == 9 )
			SetDvar( "mw2coop_qa_pad2", "0 0 0 0 0 0 0 0" );
		SetDvar( "mw2coop_qa_shot", "ck-" + i );
		report( "cook " + i + ": p2 hp " + level.player2.health + " alive " + isalive( level.player2 ) + " down " + level.player2 ent_flag( "coop_downed" ) + " yaw " + int( level.player2 GetPlayerAngles()[ 1 ] ) + " pitch " + int( level.player2 GetPlayerAngles()[ 0 ] ) + " roll " + int( level.player2 GetPlayerAngles()[ 2 ] ) + " stance " + level.player2 GetStance() );
		wait 1;
	}
	report( "done" );
}

killp2()
{
	level.mw2coop_no_follow = true;
	wait 8;
	report( "player 2 gets Kill(), hp " + level.player2.health );
	level.player2 Kill( level.player2.origin, level.player );
	for ( i = 0; i < 26; i++ )
	{
		SetDvar( "mw2coop_qa_shot", "kp-" + i );
		report( "kill " + i + ": p2 hp " + level.player2.health + " alive " + isalive( level.player2 ) + " down " + level.player2 ent_flag( "coop_downed" ) + " yaw " + int( level.player2 GetPlayerAngles()[ 1 ] ) + " pitch " + int( level.player2 GetPlayerAngles()[ 0 ] ) + " roll " + int( level.player2 GetPlayerAngles()[ 2 ] ) + " stance " + level.player2 GetStance() );
		wait 1;
	}
	report( "done" );
}

failspindown()
{
	level.mw2coop_no_follow = true;
	if ( GetDvar( "mw2coop_qa_failed" ) == "1" )
	{
		wait 20;
		report( "done" );
		return;
	}
	wait 10;
	SetDvar( "mw2coop_qa_failed", "1" );
	report( "player 2 goes down" );
	level.player2 DoDamage( level.player2.health + 500, level.player2.origin );
	for ( i = 0; i < 10; i++ )
	{
		SetDvar( "mw2coop_qa_shot", "fd-" + i );
		report( "down " + i + ": p2 yaw " + int( level.player2 GetPlayerAngles()[ 1 ] ) + " pitch " + int( level.player2 GetPlayerAngles()[ 0 ] ) + " roll " + int( level.player2 GetPlayerAngles()[ 2 ] ) );
		wait 0.5;
	}
	report( "failing the mission now" );
	level thread maps\_mw2coop::mission_failed();
	for ( i = 10; i < 26; i++ )
	{
		SetDvar( "mw2coop_qa_shot", "fd-" + i );
		report( "fail " + i + ": p2 yaw " + int( level.player2 GetPlayerAngles()[ 1 ] ) + " roll " + int( level.player2 GetPlayerAngles()[ 2 ] ) );
		wait 0.5;
	}
}

// (split screen, any level; player 2's fake pad walks forward: tools/qa_native.sh with MW2COOP_KEEP_FAKEPAD=1 and a fakepad.txt "0 32767 0 0 0 0 0") about 30 s into the mission it is failed; after the
// restart player 2 must still walk: his origin is reported every second and checked at the end (issue 22: after a restart from a checkpoint player 2 could not be controlled)
failwalk()
{
	level.mw2coop_no_follow = true;
	if ( GetDvar( "mw2coop_qa_failed" ) != "1" )
	{
		wait_intro();
		wait 4;
		first = level.player2.origin;
		yaw0 = level.player2 GetPlayerAngles()[ 1 ];
		moved = 0;
		turned = 0;
		for ( i = 0; i < 28; i++ )
		{
			wait 1;
			yaw1 = level.player2 GetPlayerAngles()[ 1 ];
			turned += abs( AngleClamp180( yaw1 - yaw0 ) );
			yaw0 = yaw1;
			moved = max( moved, Distance( first, level.player2.origin ) );
		}
		report( "player 2 before the failure: moved " + int( moved ) + ", turned " + int( turned ) + " in 28 s" );
		check( moved > 60 || turned > 30, "player 2 answers his pad before the failure" );
		SetDvar( "mw2coop_qa_failed", "1" );
		report( "failing the mission now" );
		level thread maps\_mw2coop::mission_failed();
		return;
	}
	wait_intro();
	wait 4;
	start = level.player2.origin;
	yaw0 = level.player2 GetPlayerAngles()[ 1 ];
	report( "after the restart, player 2 starts at " + int( start[ 0 ] ) + " " + int( start[ 1 ] ) + " yaw " + int( yaw0 ) );
	moved = 0;
	turned = 0;
	for ( i = 0; i < 12; i++ )
	{
		wait 1;
		yaw1 = level.player2 GetPlayerAngles()[ 1 ];
		turned += abs( AngleClamp180( yaw1 - yaw0 ) );
		yaw0 = yaw1;
		moved = max( moved, Distance( start, level.player2.origin ) );
		report( "after the restart, second " + i + ": player 2 at " + int( level.player2.origin[ 0 ] ) + " " + int( level.player2.origin[ 1 ] ) + " yaw " + int( level.player2 GetPlayerAngles()[ 1 ] ) + ", health " + level.player2.health );
	}
	check( moved > 60 || turned > 30, "player 2 answers his pad after the restart (moved " + int( moved ) + ", turned " + int( turned ) + " in 12 s)" );
	report( "done" );
}

failspin()
{
	level.mw2coop_no_follow = true;
	if ( GetDvar( "mw2coop_qa_failed" ) != "1" )
	{
		wait 12;
		SetDvar( "mw2coop_qa_failed", "1" );
		report( "failing the mission now" );
		if ( GetDvar( "mw2coop_qa_kill" ) == "1" || GetDvar( "mw2coop_qa" ) == "failspinkill" )
		{
			// both players really die (the scripted kill of a level: the death view, the downed state of player 2)
			SetDvar( "mw2coop_god", "0" );
			wait 0.3;
			foreach ( p in level.players )
			{
				p EnableDeathShield( false );
				p EnableHealthShield( false );
				p EnableInvulnerability();
				p DisableInvulnerability();
			}
			level.player2 Kill( level.player2.origin );
			for ( i = 0; i < 5; i++ )
			{
				SetDvar( "mw2coop_qa_shot", "fk-" + i );
				wait 0.5;
			}
			level.player Kill( level.player.origin );
			for ( i = 5; i < 20; i++ )
			{
				SetDvar( "mw2coop_qa_shot", "fk-" + i );
				report( "death " + i + ": p2 yaw " + int( level.player2 GetPlayerAngles()[ 1 ] ) + " hp " + level.player2.health );
				wait 0.5;
			}
		}
		else
			level thread maps\_mw2coop::mission_failed();
		return;
	}
	for ( i = 0; i < 20; i++ )
	{
		wait 1;
		SetDvar( "mw2coop_qa_shot", "fs-" + i );
		report( "after the restart, frame " + i + ": p2 yaw " + int( level.player2 GetPlayerAngles()[ 1 ] ) + " pitch " + int( level.player2 GetPlayerAngles()[ 0 ] ) + ", p1 yaw " + int( level.player GetPlayerAngles()[ 1 ] ) );
	}
	report( "done" );
}

allyrestore()
{
	level.mw2coop_no_follow = true;
	if ( GetDvar( "mw2coop_qa_failed" ) != "1" )
	{
		wait 8;
		SetDvar( "mw2coop_qa_failed", "1" );
		SetDvar( "mw2coop_cp_start", level.start_point );
		maps\_mw2coop::snapshot_allies();
		text = GetDvar( "mw2coop_cp_allies" );
		entries = StrTok( text, ";" );
		report( "snapshot: " + entries.size + " allies; first '" + entries[ 0 ] + "'" );
		// the first ally's recorded place: 1500 units along x from where he stands
		parts = StrTok( entries[ 0 ], "|" );
		ai = undefined;
		foreach ( guy in GetAIArray( "allies" ) )
			if ( !isdefined( ai ) && IsAlive( guy ) )
				ai = guy;
		fake = ( ai.origin[ 0 ] + 1500, ai.origin[ 1 ], ai.origin[ 2 ] );
		entries[ 0 ] = parts[ 0 ] + "|" + parts[ 1 ] + "|" + maps\_mw2coop::spot_int( fake[ 0 ] ) + "_" + maps\_mw2coop::spot_int( fake[ 1 ] ) + "_" + maps\_mw2coop::spot_int( fake[ 2 ] ) + "_0";
		text = "";
		foreach ( n, e in entries )
		{
			if ( n > 0 )
				text += ";";
			text += e;
		}
		SetDvar( "mw2coop_cp_allies", text );
		SetDvar( "mw2coop_qa_fake", maps\_mw2coop::spot_int( fake[ 0 ] ) + " " + maps\_mw2coop::spot_int( fake[ 1 ] ) );
		report( "failing the mission, the first ally's recorded place is " + vec_int( fake ) );
		level thread maps\_mw2coop::mission_failed();
		return;
	}
	wait 8;
	want = StrTok( GetDvar( "mw2coop_qa_fake" ), " " );
	target = ( maps\_mw2coop::spot_value( want[ 0 ] ), maps\_mw2coop::spot_value( want[ 1 ] ), 0 );
	near = 99999;
	foreach ( guy in GetAIArray( "allies" ) )
		if ( Distance2D( guy.origin, target ) < near )
			near = Distance2D( guy.origin, target );
	check( near < 200, "an ally stands at the recorded place after the restart (the nearest is " + int( near ) + " units from it)" );
	report( "done" );
}

squadgather()
{
	for ( round = 0; round < 3; round++ )
	{
		if ( round == 0 )
			wait 3;
		else
			wait 7;
		far = 0;
		text = "";
		foreach ( ai in GetAIArray( "allies" ) )
		{
			if ( !IsAlive( ai ) )
				continue;
			near = 99999;
			foreach ( player in level.players )
				if ( Distance2D( ai.origin, player.origin ) < near )
					near = Distance2D( ai.origin, player.origin );
			if ( near > 1500 )
				far++;
			text += ai.classname + "/" + isdefined( ai.script_noteworthy ) + ":" + int( near ) + " ";
		}
		report( "allies at " + ( round * 7 + 3 ) + " s, " + far + " farther than 1500 from every player: " + text );
	}
	report( "done" );
}

// airspeed: (native split screen, airport) after the elevator both players walk forward on their pads for 6 s, 12 s after player 2 is let go: how far each got (the game
// slows player 1 to the crew's pace; player 2 must not run away from him).
airspeed()
{
	level.mw2coop_no_follow = true;
	level.mw2coop_no_leash = true;
	while ( !isdefined( level.mw2coop_elevator_node ) )
		wait 0.5;
	while ( isdefined( level.mw2coop_elevator_node ) )
		wait 0.5;
	wait 12;
	a1 = level.player.origin;
	a2 = level.player2.origin;
	t0 = GetTime();
	SetDvar( "mw2coop_qa_pad1", "0 32767 0 0 0 0 0 0" );
	SetDvar( "mw2coop_qa_pad2", "0 32767 0 0 0 0 0 0" );
	wait 6;
	secs = ( GetTime() - t0 ) / 1000;
	d1 = Distance2D( level.player.origin, a1 );
	d2 = Distance2D( level.player2.origin, a2 );
	SetDvar( "mw2coop_qa_pad1", "0 0 0 0 0 0 0 0" );
	SetDvar( "mw2coop_qa_pad2", "0 0 0 0 0 0 0 0" );
	report( "airspeed: in " + secs + " s player 1 walked " + int( d1 ) + " units (" + int( d1 / secs ) + "/s), player 2 " + int( d2 ) + " units (" + int( d2 / secs ) + "/s)" );
	report( "done" );
}

// Both players' stealth state: the levels made it for level.player only, so player 2 had no _stealth struct and the
// engine's default sight distance (enemies saw him from far away).
qa_vis( player )
{
	dist = player.maxVisibleDist;
	if ( !isdefined( dist ) )
		dist = "undefined";
	return "stealth state " + isdefined( player._stealth ) + ", maxVisibleDist " + dist;
}

stealthp2()
{
	check( level.players.size == 2 && isdefined( level.player2 ), "two players (" + level.players.size + ")" );
	for ( i = 0; i < 6; i++ )
	{
		wait 2;
		report( "t " + ( ( i + 1 ) * 2 ) + ": p1 " + qa_vis( level.player ) + " | p2 " + qa_vis( level.player2 ) );
	}
	check( isdefined( level.player2._stealth ), "player 2 has the stealth state" );
	check( isdefined( level.player2.maxVisibleDist ) && level.player2.maxVisibleDist < 4000, "player 2's maxVisibleDist is the stealth value, not the default" );
}

heldweapons()
{
	check( level.players.size == 2 && isdefined( level.player2 ), "two players (" + level.players.size + ")" );
	empty = 0;
	run = 0;
	max_run = 0;
	for ( i = 0; i < 20; i++ )
	{
		wait 2;
		c1 = level.player GetCurrentWeapon();
		c2 = level.player2 GetCurrentWeapon();
		report( "t " + ( ( i + 1 ) * 2 ) + ": p1 holds " + c1 + " (" + level.player GetWeaponsListPrimaries().size + " primaries), p2 holds " + c2 + " (" + level.player2 GetWeaponsListPrimaries().size + " primaries), p2 linked " + level.player2 IsLinked() );
		if ( c1 != "none" && c2 == "none" && !level.player2 IsLinked() )
		{
			empty++;
			run++;	// (sync_weapons answers empty hands after 1 s: one sample may fall in that second, two in a row may not)
			if ( run > max_run )
				max_run = run;
			if ( GetDvar( "mw2coop_qa_try" ) == "1" && empty == 12 )
			{
				p2 = level.player2;
				report( "p2 state: linked " + p2 IsLinked() + ", stance " + p2 GetStance() + ", on ground " + p2 IsOnGround() + ", weapons " + p2 GetWeaponsListAll().size );
				p2 EnableWeapons();
				p2 SwitchToWeapon( c1 );
				wait 0.6;
				report( "after EnableWeapons + SwitchToWeapon: p2 holds " + p2 GetCurrentWeapon() );
				p2 AllowFire( true );
				p2 AllowADS( true );
				p2 SwitchToWeapon( c1 );
				wait 0.6;
				report( "after AllowFire/ADS + SwitchToWeapon: p2 holds " + p2 GetCurrentWeapon() );
				// the pad's Y (weapnext) on player 2's fake pad: what he does himself
				SetDvar( "mw2coop_qa_pad2", "0 0 0 0 32768 0 0 0" );
				wait 0.5;
				SetDvar( "mw2coop_qa_pad2", "0 0 0 0 0 0 0 0" );
				wait 1;
				report( "after his own weapnext (pad Y): p2 holds " + p2 GetCurrentWeapon() );
			}
		}
		else
			run = 0;
	}
	check( max_run < 2, "player 2 never stood with empty hands for two samples in a row while player 1 held a weapon (" + empty + " samples, " + max_run + " in a row)" );
	report( "done" );
}

// The Gulag fly-in: player 2 looks at player 1 (his view controller's target), the angles of the bodies are reported for a few seconds.
gulagturn()
{
	check( level.players.size == 2 && isdefined( level.player2 ), "two players (" + level.players.size + ")" );
	p1 = level.player;
	p2 = level.player2;
	for ( t = 0; t < 30 && !isdefined( level.mw2coop_view_controller2 ); t += 0.25 )
		wait 0.25;
	check( isdefined( level.mw2coop_view_controller2 ), "player 2 has his own view controller in the helicopter" );
	controller = level.mw2coop_view_controller2;
	// his seat lets him turn about 40 degrees: wide limits, so that he can look at the player beside him
	p2 Unlink();
	p2 PlayerLinkToDelta( controller, "TAG_aim", 1, 170, 170, 80, 80, true );
	controller ClearTargetEntity();
	controller SetTargetEntity( p1 );
	for ( i = 0; i < 12; i++ )
	{
		wait 1;
		heli = level.player_heli;
		hy = 0;
		if ( isdefined( heli ) )
			hy = heli.angles[ 1 ];
		SetDvar( "mw2coop_qa_shot", "gt-" + i );
		report( "heli yaw " + int( hy ) + ", p1 yaw " + int( p1 GetPlayerAngles()[ 1 ] ) + " origin " + vec_int( p1.origin ) + ", p2 yaw " + int( p2 GetPlayerAngles()[ 1 ] ) + " origin " + vec_int( p2.origin ) + ", p1 linked " + p1 IsLinked() + ", p2 linked " + p2 IsLinked() );
	}
	report( "done" );
}

// Oil Rig, the lower hostage room: two doors, one player at each (see the list at the top). The stock co-op breach starts the other player's breach at his own door when he stands
// in that door's volume (the player_breach function of the breach script, level.breach_groups).
qa_stand_at_door( player, breach )
{
	// the door's use trigger needs the player inside it and looking at it: from the door volume's side, step toward the trigger until the player touches it
	away = ( breach.door_volume.origin[ 0 ] - breach.trigger.origin[ 0 ], breach.door_volume.origin[ 1 ] - breach.trigger.origin[ 1 ], 0 );
	away = VectorNormalize( away );
	toward = ( 0 - away[ 0 ], 0 - away[ 1 ], 0 );
	yaw = VectorToAngles( toward )[ 1 ];
	foreach ( dist in strtok( "90 70 55 40 28 16 6", " " ) )
	{
		pos = breach.trigger.origin + away * int( dist );
		floor = PlayerPhysicsTrace( ( pos[ 0 ], pos[ 1 ], breach.door_volume.origin[ 2 ] + 40 ), ( pos[ 0 ], pos[ 1 ], breach.door_volume.origin[ 2 ] - 200 ) );
		player SetOrigin( floor );
		player SetPlayerAngles( ( 0, yaw, 0 ) );
		wait 0.25;
		if ( player IsTouching( breach.trigger ) && player IsTouching( breach.door_volume ) )
		{
			report( player_name( player ) + " stands " + dist + " units from the door's trigger at " + vec_int( floor ) );
			return;
		}
	}
	report( player_name( player ) + " could not be placed inside both the trigger and the volume" );
}

qa_breach_watch()
{
	level.mw2coop_qa_breaching = 0;
	for ( ;; )
	{
		level waittill( "breaching" );
		level.mw2coop_qa_breaching++;
		level.mw2coop_qa_breaching_time = GetTime();
	}
}

// issue 13 (a Steam client loses his weapon at door assaults): every change of each player's current weapon, his number of primaries and whether he is linked, with the time (report lines)
weapon_change_watch()
{
	last = [];
	for ( ;; )
	{
		foreach ( player in level.players )
		{
			n = 1;
			if ( player == level.player2 )
				n = 2;
			w = player GetCurrentWeapon();
			c = player GetWeaponsListPrimaries().size;
			l = player IsLinked();
			now = w + ", primaries " + c + ", linked " + l;
			if ( !isdefined( last[ n ] ) || last[ n ] != now )
			{
				report( "weapon watch: player " + n + ": " + now );
				last[ n ] = now;
			}
		}
		wait 0.2;
	}
}

// oilrigwait: player 1 at the lower room's door, player 2 left at the water exit (as in the tester's log), nothing switched off: the room's guards come (the flag a player
// sets by walking past the trigger), and for 25 s the stock sight check is run against player 1 each second to show the case is exercised; the mission must not fail.
oilrigwait()
{
	check( level.players.size == 2 && isdefined( level.player2 ), "two players (" + level.players.size + ")" );
	level.mw2coop_no_follow = true;
	level.mw2coop_no_leash = true;
	p1 = level.player;
	wait 4;
	check( isdefined( level.breach_groups ) && isdefined( level.breach_groups[ 0 ] ), "the level has breach 0" );
	a = level.breach_groups[ 0 ];
	// what breaks the stealth, when (the level's own alarms): the flags and the notify the fail paths wait for
	foreach ( f in strtok( "player_alerted_railing player_broke_stealth railing_patroller_dead oilrig_mission_failed", " " ) )
		level thread oilrigwait_flag( f );
	level thread oilrigwait_notify( "stealth_broken" );
	report( "before the door: player 1 ignoreme " + p1.ignoreme + ", player 2 ignoreme " + level.player2.ignoreme );
	qa_stand_at_door( p1, a );
	report( "at the door: player 1 ignoreme " + p1.ignoreme + ", in the door's volume " + p1 IsTouching( a.door_volume ) );
	flag_set( "player_at_lower_breach" );
	wait 2;
	check( isdefined( level.oilrigStealth ) && level.oilrigStealth, "the level's stealth monitor is on" );
	seen = 0;
	for ( t = 0; t < 25 && !flag( "oilrig_mission_failed" ); t++ )
	{
		wait 1;
		guards = 0;
		foreach ( ai in GetAIArray( "axis" ) )
		{
			if ( !IsAlive( ai ) )
				continue;
			guards++;
			if ( within_fov( ai.origin, ai.angles, p1.origin + ( 0, 0, 32 ), level.cosine[ "45" ] ) && sighttracepassed( ai.origin + ( 0, 0, 32 ), p1.origin + ( 0, 0, 32 ), false, ai ) && distancesquared( ai.origin, p1.origin ) < level.stealthDistanceSquared )
			{
				seen++;
				break;
			}
		}
		if ( t % 5 == 0 )
			report( "t " + t + ": guards alive " + guards + ", player 1 " + vec_int( p1.origin ) + " at the door " + p1 IsTouching( a.door_volume ) + ", seconds a guard had him in the stock check " + seen );
	}
	report( "seconds in which a guard had player 1 in the stock check: " + seen + ", death quote '" + GetDvar( "ui_deadquote" ) + "'" );
	check( !flag( "oilrig_mission_failed" ), "the mission did not fail while player 1 waited at the door (" + t + " s)" );
	check( p1 IsTouching( a.door_volume ), "player 1 is still at the door" );
}

oilrigwait_flag( f )
{
	level endon( "mw2coop_qa_done" );
	flag_wait( f );
	report( "flag " + f + " set at " + GetTime() );
}

oilrigwait_notify( n )
{
	level endon( "mw2coop_qa_done" );
	level waittill( n );
	report( "level notify " + n + " at " + GetTime() );
}

oilrigdoors()
{
	thread weapon_change_watch();
	check( level.players.size == 2 && isdefined( level.player2 ), "two players (" + level.players.size + ")" );
	level.mw2coop_no_follow = true;
	p1 = level.player;
	p2 = level.player2;
	wait 4;
	check( isdefined( level.breach_groups ) && isdefined( level.breach_groups[ 0 ] ) && isdefined( level.breach_groups[ 1 ] ), "the level has breaches 0 and 1" );
	a = level.breach_groups[ 0 ];
	b = level.breach_groups[ 1 ];
	check( isdefined( a.door_volume.script_breachgroup ) && a.door_volume.script_breachgroup == b.door_volume.script_breachgroup, "breaches 0 and 1 are doors of one room (group " + a.door_volume.script_breachgroup + ")" );
	report( "doors: 0 volume " + vec_int( a.door_volume.origin ) + " trigger " + vec_int( a.trigger.origin ) + ", 1 volume " + vec_int( b.door_volume.origin ) + " trigger " + vec_int( b.trigger.origin ) );
	thread qa_breach_watch();
	// the stealth part of the level (a patroller, the room's guards) fails the mission as soon as it notices a player: the bots are not seen
	p1.ignoreme = true;
	p2.ignoreme = true;
	// the level's stealth monitor alerts every guard that can see level.player nearby (a patroller sees the door): switched off for the test
	level notify( "oilrig_stealth_monitor_off" );
	level.oilrigStealth = false;
	scen = GetDvar( "mw2coop_qa" );
	other_door = b;
	if ( scen == "oilrigdoors3" )
		other_door = a;
	qa_stand_at_door( p1, a );
	qa_stand_at_door( p2, other_door );
	wait 1.5;
	check( p1 IsTouching( a.door_volume ) && p2 IsTouching( other_door.door_volume ), "each player stands in his door's volume (p1 " + vec_int( p1.origin ) + ", p2 " + vec_int( p2.origin ) + ")" );
	check( p1 IsTouching( a.trigger ) && p2 IsTouching( other_door.trigger ), "each player is inside his door's use trigger" );
	presser = p1;
	if ( scen == "oilrigdoors2" )
		presser = p2;
	// the lower breach is armed by the level only after the railing patroller is dead (a friendly kills him on his own schedule): wait for the flags instead of pressing early
	// the bots are teleported, they never walk through the triggers that a player passes: set the same flags (the friendly then executes the patroller)
	flag_set( "player_at_lower_breach" );
	flag_set( "player_alerted_railing" );
	for ( t = 0; t < 90 && !( flag( "player_at_lower_breach" ) && flag( "railing_patroller_dead" ) ); t += 0.5 )
		wait 0.5;
	report( "flags after " + t + " s: player_at_lower_breach " + flag( "player_at_lower_breach" ) + ", railing_patroller_dead " + flag( "railing_patroller_dead" ) );
	wait 3;
	for ( n = 0; n < 6 && level.mw2coop_qa_breaching == 0; n++ )
	{
		press( presser, "activate", 600 );
		wait 1;
	}
	check( level.mw2coop_qa_breaching > 0, "a breach started (" + level.mw2coop_qa_breaching + " breaching notifies)" );
	wait 1;
	report( "during the breach: p1 linked " + p1 IsLinked() + ", p2 linked " + p2 IsLinked() + ", breaching notifies " + level.mw2coop_qa_breaching + ", enemies alive " + level.breachEnemies_alive );
	check( p1 IsLinked() && p2 IsLinked(), "both players are in a breach animation" );
	// the bots do not shoot, and the enemies execute the hostages seconds after the door goes (the slow motion makes the waits long): the room's enemies are killed for player 1 at once
	killed = 0;
	foreach ( ai in GetAIArray( "axis" ) )
	{
		if ( IsAlive( ai ) && ai IsTouching( a.room_volume ) )
		{
			ai DoDamage( ai.health + 500, p1.origin, p1 );
			killed++;
		}
	}
	report( "enemies in the room killed: " + killed + ", hostages left: " + GetAIArray( "neutral" ).size );
	for ( t = 0; t < 30 && ( p1 IsLinked() || p2 IsLinked() ); t += 0.5 )
		wait 0.5;
	check( !p1 IsLinked() && !p2 IsLinked(), "both are free again after the breach (" + t + " s)" );
	report( "weapons after the breach: p1 " + p1 GetCurrentWeapon() + ", p2 " + p2 GetCurrentWeapon() );
	check( p1 GetCurrentWeapon() != "none" && p2 GetCurrentWeapon() != "none", "both hold a weapon after the breach (Smelt1993, issue 9: a player without a weapon in hand at the breach)" );
	for ( i = 0; i < 16; i++ )
	{
		wait 0.5;
		if ( i % 4 == 0 )
			report( "after the breach: game time " + GetTime() + ", timescale " + GetDvar( "timescale" ) + ", p1 " + vec_int( p1.origin ) + ", p2 " + vec_int( p2.origin ) + ", failed " + flag( "oilrig_mission_failed" ) );
	}
	check( IsAlive( p1 ) && IsAlive( p2 ), "both alive" );
	check( !flag( "oilrig_mission_failed" ), "the mission did not fail (a hostage was not killed)" );
	check( a.room_volume.breached && b.room_volume.breached, "the room is breached" );
	check( flag( a.room_volume.script_flag ), "the room is cleared (flag " + a.room_volume.script_flag + ")" );
	report( "end: breaching notifies " + level.mw2coop_qa_breaching + ", p1 " + vec_int( p1.origin ) + " p2 " + vec_int( p2.origin ) );
	report( "done" );
}

// The Only Easy Day... opening (default start). Names no oilrig script (not in other zones).
// The strike is forced through player.mw2coop_qa_melee (the bot cannot aim at the guard).
oilrig()
{
	check( level.players.size == 2 && isdefined( level.player2 ), "two players (" + level.players.size + ")" );
	p1 = level.player;
	p2 = level.player2;
	flag_wait( "player_attached_to_sdv" );
	wait 1;
	check( p1 IsLinked() && p2 IsLinked(), "both on an SDV (linked " + p1 IsLinked() + " " + p2 IsLinked() + ")" );
	d = Distance( p1.origin, p2.origin );
	report( "underwater, players " + int( d ) + " units apart" );
	flag_wait( "player_is_done_swimming" );
	wait 2;
	d = Distance( p1.origin, p2.origin );
	check( p1 IsLinked() && p2 IsLinked() && d < 600, "both surfaced and swimming (" + int( d ) + " units apart, p1 " + p1.origin + " p2 " + p2.origin + ")" );
	killer = p1;
	other = p2;
	if ( GetDvar( "mw2coop_qa" ) == "oilrig2" )
	{
		killer = p2;
		other = p1;
	}
	wait 3;
	killer.mw2coop_qa_melee = true;
	for ( t = 0; t < 60 && !flag( "player_done_being_helped_from_water" ); t += 0.25 )
		wait 0.25;
	check( flag( "player_done_being_helped_from_water" ), "the killer is out of the water" );
	for ( t = 0; t < 40 && !isdefined( other.mw2coop_out_of_water ); t += 0.25 )
		wait 0.25;
	check( isdefined( other.mw2coop_out_of_water ), "the other player is pulled out after him (" + t + " s)" );
	wait 1;
	d = Distance( p1.origin, p2.origin );
	check( !p1 IsLinked() && !p2 IsLinked() && d < 300 && abs( p1.origin[ 2 ] - p2.origin[ 2 ] ) < 30, "both out on the platform together (" + int( d ) + " units, p1 " + p1.origin + " linked " + p1 IsLinked() + ", p2 " + p2.origin + " linked " + p2 IsLinked() + ")" );
	check( IsAlive( p1 ) && IsAlive( p2 ), "both alive" );
	report( "done" );
}

// Second Sun opening (default start). Names no dcemp script (not in other zones).
dcemp()
{
	check( level.players.size == 2 && isdefined( level.player2 ), "two players (" + level.players.size + ")" );
	p1 = level.player;
	p2 = level.player2;
	flag_wait( "crash_actors_ready" );
	wait 0.5;
	d = Distance( p1 GetEye(), p2 GetEye() );
	check( p1 IsLinked() && p2 IsLinked() && d > 16 && d < 90, "both in the wreck (eyes " + int( d ) + " units apart)" );
	flag_wait( "give_player_weapon" );
	wait 1;
	check( p1 HasWeapon( "m4m203_eotech" ) && p2 HasWeapon( "m4m203_eotech" ), "both got a rifle" );
	if ( level.script != "dcemp" )
	{
		// dcburning ends here: the crash site is its last scene
		flag_wait( "end_sequence_starting" );
		check( p1 IsLinked() && p2 IsLinked(), "both still in the wreck at the end" );
		report( "done" );
		return;
	}
	flag_wait( "iss_fx" );
	wait 3;
	d = Distance( p1 GetEye(), p2 GetEye() );
	check( p1 IsLinked() && p2 IsLinked() && d < 150, "both at the ISS (" + int( d ) + " units apart)" );
	flag_wait( "iss_done" );
	wait 4;
	d = Distance( p1 GetEye(), p2 GetEye() );
	check( p1 IsLinked() && p2 IsLinked() && d < 120, "both back in the wreck (" + int( d ) + " units apart)" );
	for ( t = 0; t < 60 && ( p1 IsLinked() || p2 IsLinked() ); t += 0.25 )
		wait 0.25;
	wait 1;
	d = Distance( p1.origin, p2.origin );
	check( !p1 IsLinked() && !p2 IsLinked() && d < 150, "both out of the wreck (" + int( d ) + " units, after " + t + " s)" );
	check( IsAlive( p1 ) && IsAlive( p2 ), "both alive" );
	report( "done" );
}

// Second Sun street (start "street"). Names no dcemp script (not in other zones).
street()
{
	check( level.players.size == 2 && isdefined( level.player2 ), "two players (" + level.players.size + ")" );
	p1 = level.player;
	p2 = level.player2;
	hide = getEntArrayWithFlag( "street_player_hide" );
	check( hide.size > 0, "hiding place trigger found (" + hide.size + ")" );
	if ( !hide.size )
		return;
	spot = hide[ 0 ].origin;
	foreach ( guy in level.team )
		guy ent_flag_wait( "street_hide" );
	report( "team hidden, hiding place at " + spot );
	level.mw2coop_no_follow = true;	// the bot teleports player 1: player 2 must not follow
	p1 SetOrigin( PlayerPhysicsTrace( spot + ( 0, 0, 40 ), spot - ( 0, 0, 200 ) ) );
	wait 2;
	check( isdefined( p1.mw2coop_street_hidden ) && !flag( "street_safe" ), "player 1 hidden, street not safe while player 2 is out" );
	p2 SetOrigin( PlayerPhysicsTrace( spot + ( 24, 0, 40 ), spot + ( 24, 0, -200 ) ) );
	for ( t = 0; t < 5 && !flag( "street_safe" ); t += 0.1 )
		wait 0.1;
	check( flag( "street_safe" ), "street safe once both are hidden" );
	wait 3;
	check( IsAlive( p1 ) && IsAlive( p2 ), "both alive" );
	report( "done" );
}

// No Russian ending (start "escape"). Names no airport script (not in other zones): the bot puts
// both players by the van's ending node and watches Makarov's shots.
nrend()
{
	check( level.players.size == 2 && isdefined( level.player2 ), "two players (" + level.players.size + ")" );
	p1 = level.player;
	p2 = level.player2;
	node = getstruct( "escape_ending_node", "targetname" );
	check( isdefined( node ), "ending node found" );
	level.mw2coop_no_follow = true;
	// follow Makarov along the escape (he waits for the player), then stand by the van
	for ( t = 0; t < 150 && !flag( "escape_sequence_go" ); t += 0.5 )
	{
		foreach ( i, player in level.players )
		{
			if ( player IsLinked() || !isdefined( level.makarov ) )
				continue;
			if ( !flag( "escape_sequence_reach" ) )
			{
				back = AnglesToForward( ( 0, level.makarov.angles[ 1 ], 0 ) ) * -140;
				right = AnglesToRight( ( 0, level.makarov.angles[ 1 ], 0 ) ) * ( 40 * ( i * 2 - 1 ) );
				at = level.makarov.origin + back + right;
				if ( Distance( player.origin, at ) > 220 )
					player SetOrigin( PlayerPhysicsTrace( at + ( 0, 0, 40 ), at - ( 0, 0, 100 ) ) );
			}
			else if ( GetDvar( "mw2coop_qa" ) == "nrend2" && player == level.player && isdefined( level.escape_van_dummy ) )
			{
				// where the close ending's player rig starts, facing the van
				van = level.escape_van_dummy;
				anime = level.scr_anim[ "player_ending" ][ "end_player_shot" ];
				at = GetStartOrigin( van GetTagOrigin( "origin_animate_jnt" ), van GetTagAngles( "origin_animate_jnt" ), anime );
				if ( Distance( player.origin, at ) > 20 )
					player SetOrigin( PlayerPhysicsTrace( at + ( 0, 0, 40 ), at - ( 0, 0, 100 ) ) );
				player SetPlayerAngles( VectorToAngles( ( van.origin[ 0 ], van.origin[ 1 ], at[ 2 ] ) - at ) );
			}
			else
			{
				right = AnglesToRight( ( 0, node.angles[ 1 ], 0 ) );
				at = node.origin + right * ( 50 * ( i * 2 - 1 ) );
				if ( Distance( player.origin, at ) > 60 )
					player SetOrigin( PlayerPhysicsTrace( at + ( 0, 0, 40 ), at - ( 0, 0, 100 ) ) );
			}
		}
		wait 0.5;
	}
	check( flag( "escape_sequence_go" ), "the ending starts (" + t + " s)" );
	for ( t = 0; t < 60 && ( !isdefined( level.mw2coop_ending_left ) || level.mw2coop_ending_left > 0 ); t += 0.25 )
		wait 0.25;
	check( isdefined( level.mw2coop_ending_left ) && level.mw2coop_ending_left == 0, "Makarov shot both players (" + t + " s)" );
	wait 3;
	check( p1 IsLinked() && p2 IsLinked(), "both down (linked " + p1 IsLinked() + " " + p2 IsLinked() + ")" );
	report( "health p1 " + p1.health + " p2 " + p2.health );
	report( "done" );
}

// Endgame knife fight (ending map, start "wakeup"). Names no knife fight script (not in other
// zones): the bot strikes through player.mw2coop_qa_melee, holds the crawl and Use buttons
// through player.mw2coop_qa_hold and throws with the "throw" notify. Only the striker is moved
// (to Shepherd); the other player is left where he got up.
// The knife model's own axes: a script model of it 24 units ahead of player 1's eye, its origin on the view's centre (the crosshair), its angles the view's
// (yaw only): turned 0, 90 and 180 degrees about up, then rolled 90, a frame of each
knifeaxis()
{
	flag_wait( "player_standing" );
	wait 12;
	p1 = level.player;
	yaw = p1 GetPlayerAngles()[ 1 ];
	p1 SetPlayerAngles( ( 0, yaw, 0 ) );
	wait 0.5;
	knife = Spawn( "script_model", p1 GetEye() + AnglesToForward( ( 0, yaw, 0 ) ) * 24 );
	knife SetModel( "weapon_commando_knife" );
	turns = [];
	turns[ 0 ] = ( 0, yaw, 0 );
	turns[ 1 ] = ( 0, yaw + 90, 0 );
	turns[ 2 ] = ( 0, yaw + 180, 0 );
	turns[ 3 ] = ( 0, yaw, 90 );
	foreach ( k, a in turns )
	{
		knife.angles = a;
		wait 0.4;
		qa_shot( "ka" + k );
	}
	report( "knifeaxis: done" );
}

// Endgame after the wake-up: both halves under the wake-up's shell shock (a frame of the whole screen), then, the shock taken off for sharp frames, player 1's
// knife hand close up from player 2's eye (from his left, his front-left, behind-left and his right), then player 1 walking seen from his left
knifehand()
{
	flag_wait( "player_standing" );
	wait 12;
	qa_shot( "khs" );
	p1 = level.player;
	p2 = level.player2;
	foreach ( player in level.players )
		player StopShellShock();
	SetBlur( 0, 0 );
	wait 3;
	yaw = p1 GetPlayerAngles()[ 1 ];
	fwd = AnglesToForward( ( 0, yaw, 0 ) );
	left = AnglesToRight( ( 0, yaw, 0 ) ) * -1;
	spots = [];
	spots[ 0 ] = left * 34 + fwd * 10;
	spots[ 1 ] = left * 30 + fwd * 30;
	spots[ 2 ] = left * 24 - fwd * 4;
	spots[ 3 ] = left * -34 + fwd * 20;
	tag = "tag_weapon_left";
	model = "weapon_commando_knife";
	for ( i = 0; i < p1 GetAttachSize(); i++ )
		if ( IsSubStr( p1 GetAttachModelName( i ), "knife" ) )
		{
			tag = p1 GetAttachTagName( i );
			model = p1 GetAttachModelName( i );
		}
	if ( GetDvar( "mw2coop_qa_knifetag" ) != "" )
		tag = GetDvar( "mw2coop_qa_knifetag" );
	models = StrTok( GetDvar( "mw2coop_qa_knifemodels" ), " " );	// (other knife models to try on the same tag, after the one attached)
	all = [];
	all[ 0 ] = model;
	foreach ( m in models )
		all[ all.size ] = m;
	start = p1.origin;
	foreach ( m, entry in all )
	{
		parts = StrTok( entry, ":" );	// "<model>[:<hand mode>]" (src/transplant.c knife_hand_mode)
		name = parts[ 0 ];
		if ( parts.size > 1 )
			SetDvar( "mw2coop_qa_knifehand", parts[ 1 ] );
		wait 1.2;
		p1 SetOrigin( start );	// (each model from the same place: the walk moved him)
		p1 SetPlayerAngles( ( 0, yaw, 0 ) );
		for ( i = p1 GetAttachSize() - 1; i >= 0; i-- )
			if ( IsSubStr( p1 GetAttachModelName( i ), "knife" ) )
				p1 Detach( p1 GetAttachModelName( i ), p1 GetAttachTagName( i ) );
		p1 Attach( name, tag, true );
		report( "knifehand " + m + ": " + entry + " on " + tag );
		if ( GetDvar( "mw2coop_qa_knifegrip" ) == "1" )
		{
			wait 1;
			tags = "j_wrist_le";
			foreach ( finger in StrTok( "thumb index mid ring pinky", " " ) )
				for ( k = 1; k <= 3; k++ )
					tags = tags + " j_" + finger + "_le_" + k;
			foreach ( t in StrTok( tags, " " ) )
			{
				o = p1 GetTagOrigin( t );
				ta = p1 GetTagAngles( t );
				f = AnglesToForward( ta );
				l = AnglesToRight( ta ) * -1;
				u = AnglesToUp( ta );
				report( "knifegrip " + m + " " + t + " o " + o[ 0 ] + " " + o[ 1 ] + " " + o[ 2 ] + " f " + f[ 0 ] + " " + f[ 1 ] + " " + f[ 2 ] + " l " + l[ 0 ] + " " + l[ 1 ] + " " + l[ 2 ] + " u " + u[ 0 ] + " " + u[ 1 ] + " " + u[ 2 ] );
			}
		}
		foreach ( k, spot in spots )
		{
			at = p1.origin + spot;
			p2 SetOrigin( PhysicsTrace( at + ( 0, 0, 48 ), at - ( 0, 0, 96 ) ) );
			wait 0.3;
			p2 SetPlayerAngles( VectorToAngles( p1 GetTagOrigin( tag ) - p2 GetEye() ) );
			wait 0.6;
			qa_shot( "kh" + m + "-" + k );
		}
		// walking, seen from his left at 50 units
		p1 SetPlayerAngles( ( 0, yaw, 0 ) );
		at = p1.origin + left * 50 + fwd * 40;
		p2 SetOrigin( PhysicsTrace( at + ( 0, 0, 48 ), at - ( 0, 0, 96 ) ) );
		wait 0.3;
		SetDvar( "mw2coop_qa_press", "1 forward 1800 " + m );
		for ( k = 0; k < 2; k++ )
		{
			wait 0.25;
			p2 SetPlayerAngles( VectorToAngles( p1 GetTagOrigin( tag ) - p2 GetEye() ) );
			wait 0.05;
			qa_shot( "kw" + m + "-" + k );
		}
		wait 1;
	}
	at = p1.origin + left * 90 + fwd * 60;
	p2 SetOrigin( PhysicsTrace( at + ( 0, 0, 48 ), at - ( 0, 0, 96 ) ) );
	p1 SetPlayerAngles( ( 0, yaw, 0 ) );
	wait 0.3;
	SetDvar( "mw2coop_qa_press", "1 forward 3000" );
	for ( k = 0; k < 3; k++ )
	{
		p2 SetPlayerAngles( VectorToAngles( p1.origin + ( 0, 0, 40 ) - p2 GetEye() ) );
		wait 0.05;
		qa_shot( "khw" + k );
	}
	report( "knifehand: done" );
}

// Endgame after the wake-up as the campaign plays it (af_chase, start "wakeup"; nothing attached by the QA): once player 2 holds ending_knife, which models
// hang on each body (the knife copy mw2coop_knife_held on j_wrist_le is the fix, ending_knife's own world model is a silenced pistol), then both face each
// other for frames of both views. Owner, 2026-11-18: "where is the knife fix?" (the fix was only made for the level ending, where knifehand ran).
knifelook()
{
	flag_wait( "player_standing" );
	p1 = level.player;
	p2 = level.player2;
	level.mw2coop_no_follow = true;
	for ( t = 0; t < 30 && p2 GetCurrentWeapon() != "ending_knife"; t += 0.5 )
		wait 0.5;
	wait 1;
	foreach ( n, player in level.players )
	{
		list = "";
		held = false;
		for ( i = 0; i < player GetAttachSize(); i++ )
		{
			list = list + " " + player GetAttachModelName( i ) + "@" + player GetAttachTagName( i );
			if ( player GetAttachModelName( i ) == "mw2coop_knife_held" )
				held = true;
		}
		report( "knifelook: player " + ( n + 1 ) + " holds " + player GetCurrentWeapon() + ", attached:" + list );
		check( player GetCurrentWeapon() != "ending_knife" || held, "player " + ( n + 1 ) + "'s body holds the knife copy, not the pistol model" );
	}
	foreach ( player in level.players )
		player StopShellShock();	// (the frames show the knife, not the blur)
	SetBlur( 0, 0 );
	yaw = p1 GetPlayerAngles()[ 1 ];
	fwd = AnglesToForward( ( 0, yaw, 0 ) );
	left = AnglesToRight( ( 0, yaw, 0 ) ) * -1;
	spots = [];
	spots[ 0 ] = fwd * 60 + left * 10;
	spots[ 1 ] = fwd * 45 - left * 30;
	foreach ( k, spot in spots )
	{
		at = p1.origin + spot;
		p2 SetOrigin( PhysicsTrace( at + ( 0, 0, 48 ), at - ( 0, 0, 96 ) ) );
		wait 0.3;
		p1 SetPlayerAngles( VectorToAngles( p2 GetEye() - ( 0, 0, 24 ) - p1 GetEye() ) );
		p2 SetPlayerAngles( VectorToAngles( p1 GetEye() - ( 0, 0, 24 ) - p2 GetEye() ) );
		wait 0.6;
		qa_shot( "kl-" + k );
	}
	report( "knifelook: done" );
}

// The knife in the left hand of the first-person rig, as the level holds it in the fight (af_chase_knife_fight.gsc: player_rig Attach( "weapon_commando_knife",
// "TAG_WEAPON_LEFT" ), then the turn_buckle animation): the rig's left wrist and the knife's tag at several times of that animation (still), to hold the knife
// the same way in a body's left hand
knifeframe()
{
	flag_wait( "player_standing" );
	wait 2;
	rig = spawn_anim_model( "player_rig" );
	rig.origin = level.player.origin + ( 0, 0, 2000 );
	rig.angles = ( 0, 0, 0 );
	rig Attach( "weapon_commando_knife", "TAG_WEAPON_LEFT" );
	animation = rig getanim( "turn_buckle" );
	rig SetAnim( animation, 1, 0, 0 );
	times = [];
	times[ 0 ] = 0.02;
	tags = "j_elbow_le j_wristtwist_le j_wrist_le tag_weapon_left";
	foreach ( finger in StrTok( "thumb index mid ring pinky", " " ) )
		for ( k = 1; k <= 3; k++ )
			tags = tags + " j_" + finger + "_le_" + k;
	foreach ( t in times )	// (no float() in this game's GSC)
	{
		rig SetAnimTime( animation, t );
		wait 0.2;
		foreach ( tag in StrTok( tags, " " ) )
		{
			o = rig GetTagOrigin( tag );
			ta = rig GetTagAngles( tag );
			f = AnglesToForward( ta );
			l = AnglesToRight( ta ) * -1;
			u = AnglesToUp( ta );
			report( "knifeframe t " + t + " " + tag + " o " + o[ 0 ] + " " + o[ 1 ] + " " + o[ 2 ] + " f " + f[ 0 ] + " " + f[ 1 ] + " " + f[ 2 ] + " l " + l[ 0 ] + " " + l[ 1 ] + " " + l[ 2 ] + " u " + u[ 0 ] + " " + u[ 1 ] + " " + u[ 2 ] );
		}
	}
	rig Delete();
	report( "knifeframe: done" );
}

// The hand tags of each player (world origin, forward, left, up), to place the knife model in a hand by
knife_tag_report( label )
{
	foreach ( n, player in level.players )
	{
		foreach ( tag in StrTok( "tag_weapon_right j_wrist_le j_mid_le_1 j_index_le_1 j_wrist_ri tag_inhand tag_weapon_left", " " ) )
		{
			o = player GetTagOrigin( tag );
			ta = player GetTagAngles( tag );
			f = AnglesToForward( ta );
			l = AnglesToRight( ta ) * -1;
			u = AnglesToUp( ta );
			report( "knifetag " + label + " p" + ( n + 1 ) + " " + tag + " o " + o[ 0 ] + " " + o[ 1 ] + " " + o[ 2 ] + " f " + f[ 0 ] + " " + f[ 1 ] + " " + f[ 2 ] + " l " + l[ 0 ] + " " + l[ 1 ] + " " + l[ 2 ] + " u " + u[ 0 ] + " " + u[ 1 ] + " " + u[ 2 ] );
		}
	}
}

// Endgame after the wake-up: both hold the knife; player 2 is put in front of player 1, each looks at the other: frames of both views
// (the partner's third-person knife: the body's "hold" set and the knife on tag_inhand)
knifepose()
{
	flag_wait( "player_standing" );
	wait 4;
	p1 = level.player;
	p2 = level.player2;
	fwd = AnglesToForward( ( 0, p1.angles[ 1 ], 0 ) );
	right = AnglesToRight( ( 0, p1.angles[ 1 ], 0 ) );
	at = p1.origin + fwd * 95 + right * 22;
	spot = PhysicsTrace( at + ( 0, 0, 48 ), at - ( 0, 0, 96 ) );
	p2 SetOrigin( spot );
	wait 0.2;
	// each looks at the other's waist: the whole body and both hands in the frame
	p2 SetPlayerAngles( VectorToAngles( p1.origin + ( 0, 0, 34 ) - p2 GetEye() ) );
	p1 SetPlayerAngles( VectorToAngles( p2.origin + ( 0, 0, 34 ) - p1 GetEye() ) );
	wait 1.5;
	report( "knifepose: player 1 holds " + p1 GetCurrentWeapon() + ", player 2 holds " + p2 GetCurrentWeapon() );
	foreach ( n, player in level.players )
	{
		text = "";
		for ( i = 0; i < player GetAttachSize(); i++ )
			text = text + " " + player GetAttachModelName( i ) + "@" + player GetAttachTagName( i );
		report( "knifepose: player " + ( n + 1 ) + " attachments:" + text );
	}
	knife_tag_report( "stand" );
	qa_shot( "kp1" );
	wait 9;	// (the wake-up's shell shock blur fades)
	p2 SetPlayerAngles( VectorToAngles( p1.origin + ( 0, 0, 34 ) - p2 GetEye() ) );
	p1 SetPlayerAngles( VectorToAngles( p2.origin + ( 0, 0, 34 ) - p1 GetEye() ) );
	wait 0.5;
	qa_shot( "kp2" );
	p2 SetStance( "crouch" );
	p1 SetStance( "crouch" );
	wait 2;
	knife_tag_report( "crouch" );
	qa_shot( "kp3" );
	// close-ups of player 2's hands from player 1's eye: in front, then from his right and his left side
	p2 SetStance( "stand" );
	p1 SetStance( "stand" );
	wait 1;
	fwd = AnglesToForward( ( 0, p1.angles[ 1 ], 0 ) );
	at = p1.origin + fwd * 48;
	p2 SetOrigin( PhysicsTrace( at + ( 0, 0, 48 ), at - ( 0, 0, 96 ) ) );
	sides = [];
	sides[ 0 ] = 180;
	sides[ 1 ] = 90;
	sides[ 2 ] = -90;
	foreach ( k, turn in sides )
	{
		p2 SetPlayerAngles( ( 0, p1.angles[ 1 ] + turn, 0 ) );
		wait 0.3;
		p1 SetPlayerAngles( VectorToAngles( p2.origin + ( 0, 0, 36 ) - p1 GetEye() ) );
		wait 0.7;
		qa_shot( "kpc" + k );
	}
	// player 1 walks ahead (a QA press of +forward), player 2 watches him from the side: the knife in the "hold" walk
	right = AnglesToRight( ( 0, p1.angles[ 1 ], 0 ) );
	at = p1.origin + right * 100 + AnglesToForward( ( 0, p1.angles[ 1 ], 0 ) ) * 120;
	p2 SetOrigin( PhysicsTrace( at + ( 0, 0, 48 ), at - ( 0, 0, 96 ) ) );
	p1 SetPlayerAngles( ( 0, p1.angles[ 1 ], 0 ) );
	wait 0.3;
	SetDvar( "mw2coop_qa_press", "1 forward 4500" );
	for ( k = 0; k < 6; k++ )
	{
		if ( k == 2 )
			knife_tag_report( "walk" );
		p2 SetPlayerAngles( VectorToAngles( p1.origin + ( 0, 0, 34 ) - p2 GetEye() ) );
		wait 0.05;
		qa_shot( "kpw" + k );
	}
	report( "knifepose: done" );
}

// Endgame after the wake-up: player 2's knife tried on several tags ("<tag> [model]", the dvar mw2coop_qa_knifevar, ","-separated), close-ups from player 1's eye, in
// front and from both sides
knifevar()
{
	flag_wait( "player_standing" );
	wait 12;	// (the wake-up's shell shock blur fades)
	p1 = level.player;
	p2 = level.player2;
	fwd = AnglesToForward( ( 0, p1.angles[ 1 ], 0 ) );
	at = p1.origin + fwd * 80;
	p2 SetOrigin( PhysicsTrace( at + ( 0, 0, 48 ), at - ( 0, 0, 96 ) ) );
	variants = StrTok( GetDvar( "mw2coop_qa_knifevar" ), "," );
	if ( !variants.size )
		variants = StrTok( "tag_inhand,tag_weapon_right,tag_weapon_left", "," );
	foreach ( n, v in variants )
	{
		parts = StrTok( v, " " );	// "<tag> [model]"
		model = "weapon_commando_knife";
		if ( parts.size > 1 )
			model = parts[ 1 ];
		for ( i = p2 GetAttachSize() - 1; i >= 0; i-- )
			if ( p2 GetAttachModelName( i ) == "weapon_commando_knife" || p2 GetAttachModelName( i ) == model )
				p2 Detach( p2 GetAttachModelName( i ), p2 GetAttachTagName( i ) );
		p2 Attach( model, parts[ 0 ], true );
		report( "knifevar " + n + ": " + model + " on " + parts[ 0 ] );
		turns = [];
		turns[ 0 ] = 180;
		turns[ 1 ] = 90;
		turns[ 2 ] = -90;
		foreach ( k, turn in turns )
		{
			p2 SetPlayerAngles( ( 0, p1.angles[ 1 ] + turn, 0 ) );
			wait 1.4;
			p1 SetPlayerAngles( VectorToAngles( p2.origin + ( 0, 0, 40 ) - p1 GetEye() ) );
			wait 0.3;
			qa_shot( "kv" + n + "-" + k );
		}
	}
	report( "knifevar: done" );
}

knife()
{
	check( level.players.size == 2 && isdefined( level.player2 ), "two players (" + level.players.size + ")" );
	striker = level.player;
	if ( GetDvar( "mw2coop_qa" ) == "knife2" )
		striker = level.player2;
	other = level.player;
	if ( striker == level.player )
		other = level.player2;
	foreach ( player in level.players )
	{
		player.mw2coop_qa_hold = [];
		player.mw2coop_qa_hold[ "use" ] = false;
	}
	wait 2;
	d = Distance( level.player GetEye(), level.player2 GetEye() );
	check( level.player IsLinked() && level.player2 IsLinked() && d < 140, "both wake up in the wreck (" + int( d ) + " units apart)" );
	flag_wait( "player_standing" );
	wait 2;
	check( !level.player IsLinked() && !level.player2 IsLinked(), "both up after the wake-up" );
	level.mw2coop_no_follow = true;
	for ( t = 0; t < 60 && !isdefined( level.shepherd ); t += 0.5 )
		wait 0.5;
	check( isdefined( level.shepherd ), "Shepherd there" );
	// Shepherd at the truck is hidden until the stumbling Shepherd walked there: a strike now
	// must not count (the scene would play with an invisible Shepherd)
	for ( t = 0; t < 2 && shepherd_hidden(); t += 0.25 )
		knife_strike_try( striker );
	check( !flag( "turn_buckle_start" ), "no strike while Shepherd is still hidden (hidden " + shepherd_hidden() + ")" );
	// the striker waits near the truck where he cannot see the stumbler (he stays the lead:
	// nearer to Shepherd), so only the other player's sight can start him
	for ( t = 0; t < 20 && !isdefined( level.shepherd_stumble ); t += 0.5 )
		wait 0.5;
	wait_spot = undefined;
	if ( isdefined( level.shepherd_stumble ) )
		wait_spot = sight_spot( level.shepherd_stumble, level.shepherd.origin, "150 250 350", false );
	check( isdefined( wait_spot ), "a spot by the truck without sight of the stumbling Shepherd" );
	if ( isdefined( wait_spot ) )
		striker SetOrigin( wait_spot );
	// a player walking on sets the map's trigger flag shepherd_can_run; the bot does not walk
	if ( shepherd_hidden() && flag_exist( "shepherd_can_run" ) && !flag( "shepherd_can_run" ) )
	{
		report( "QA sets shepherd_can_run (the bot does not walk)" );
		flag_set( "shepherd_can_run" );
	}
	// the stumbler runs off when a player sees him: the other player goes where he would see
	// him coming over the dunes, so the co-op sight check is what starts him
	level.mw2coop_qa_stumbler_runs = false;
	level thread stumbler_runs_watch();
	spot = undefined;
	if ( isdefined( level.shepherd_stumble ) )
		spot = sight_spot( level.shepherd_stumble, level.shepherd_stumble.origin, "150 250 350 500 700", true );
	check( isdefined( spot ), "a spot where the stumbling Shepherd can be seen" );
	if ( isdefined( spot ) )
	{
		other SetOrigin( spot );
		other SetPlayerAngles( VectorToAngles( level.shepherd_stumble GetEye() - other GetEye() ) );
		report( "other player at " + spot + ", " + int( Distance( spot, level.shepherd_stumble.origin ) ) + " units from the stumbler; sight: lead" + sight_text( level.mw2coop_lead, level.shepherd_stumble ) + " / other" + sight_text( other, level.shepherd_stumble ) );
	}
	for ( t = 0; t < 10 && shepherd_hidden() && !level.mw2coop_qa_stumbler_runs; t += 0.5 )
		wait 0.5;
	other thread watch_stumbler();
	check( level.mw2coop_qa_stumbler_runs, "the stumbling Shepherd runs off when the other player sees him (" + t + " s)" );
	for ( t = 0; t < 90 && shepherd_hidden(); t += 0.5 )
		wait 0.5;
	check( !shepherd_hidden(), "the stumbling Shepherd reached the truck (" + t + " s)" );
	for ( t = 0; t < 60 && !flag( "turn_buckle_start" ); t += 0.25 )
		knife_strike_try( striker );
	check( flag( "turn_buckle_start" ), "a player strikes Shepherd (" + t + " s)" );
	check( level.mw2coop_lead == striker, "the striker plays the scene" );
	wait 2.5;
	check( icon_hidden( striker ) && icon_hidden( other ) && GetDvar( "mw2coop_hide_names" ) == "1", "no partner icon or name on either screen" );
	check( isdefined( other.laststand ) && other.laststand && isdefined( other.mw2coop_awake ) && !other.mw2coop_awake, "the other player is down and out" );
	d = Distance( other.origin, level.shepherd.origin );
	check( d < 400, "and lies near the fight (" + int( d ) + " units from Shepherd)" );
	level thread knife_watch_report( other );

	// crawl to the revolver: alternate the two crawl buttons
	report( "crawl" );
	side = "left";
	for ( t = 0; t < 200 && !flag( "player_looks_at_knife" ) && !flag( "player_uses_knife" ); t += 0.15 )
	{
		striker.mw2coop_qa_hold[ "left" ] = side == "left";
		striker.mw2coop_qa_hold[ "right" ] = side == "right";
		if ( side == "left" )
			side = "right";
		else
			side = "left";
		wait 0.15;
	}
	striker.mw2coop_qa_hold[ "left" ] = false;
	striker.mw2coop_qa_hold[ "right" ] = false;
	report( "knife: tap Use (" + t + " s)" );
	level.mw2coop_qa_knife_out = false;
	level thread knife_out_watch();
	// pull the knife: many quick Use presses
	for ( t = 0; t < 120 && !level.mw2coop_qa_knife_out && !flag( "shepherd_killed" ); t += 0.1 )
	{
		striker.mw2coop_qa_hold[ "use" ] = !striker.mw2coop_qa_hold[ "use" ];
		wait 0.1;
	}
	striker.mw2coop_qa_hold[ "use" ] = false;
	report( "throw (" + t + " s)" );
	// aim at Shepherd and throw
	other.mw2coop_saw_kill = false;
	for ( t = 0; t < 60 && !flag( "shepherd_killed" ); t += 0.1 )
	{
		if ( other.mw2coop_awake )
			other.mw2coop_saw_kill = true;
		if ( isdefined( level.shepherd ) )
			striker SetPlayerAngles( VectorToAngles( level.shepherd GetTagOrigin( "tag_eye" ) - striker GetEye() ) );
		if ( flag( "player_aims_knife_at_shepherd" ) )
			striker notify( "throw" );
		wait 0.1;
	}
	check( flag( "shepherd_killed" ), "Shepherd killed by the striker's knife (" + t + " s)" );
	check( other.mw2coop_awake, "the other player is awake as Shepherd dies" );
	wait 3;
	check( other.mw2coop_awake, "and still awake 3 s later" );
	// issue 20 ("the second player cannot move after the crash"): a real forward press on the other player's own machine, how far does he go? By the owner's design (witness_think,
	// _mw2coop_knife.gsc: SetMoveSpeedScale 0, "he cannot move but can look around ... stays down until the end") he does not move: measured 0 units on 2026-11-11, so it is only reported
	fw = crashwalk_step( other, "forward" );
	report( player_name( other ) + " pressed forward after the kill and moved " + fw + " units (by design the witness stays down until the end)" );
	wait 27;	// Price wakes up: the witness stays awake
	check( other.laststand && IsAlive( other ), "the other player watched the whole fight, down" );
	report( "done" );
}

// shep_trace_passed()'s four traces from a player to the stumbler: fraction and surface.
sight_text( player, stumbler )
{
	text = "";
	for ( offset = 0; offset <= 48; offset += 16 )
	{
		trace = BulletTrace( player.origin + ( 0, 0, 32 ), stumbler GetEye() + ( 0, 0, offset ), false, undefined );
		text += " " + int( trace[ "fraction" ] * 100 ) + trace[ "surfacetype" ];
	}
	return text;
}

// self: a player; he keeps his eyes on the running stumbler (screenshots of him).
watch_stumbler()
{
	for ( t = 0; t < 12 && shepherd_hidden() && isdefined( level.shepherd_stumble ); t += 0.05 )
	{
		self SetPlayerAngles( VectorToAngles( level.shepherd_stumble GetEye() - self GetEye() ) );
		wait 0.05;
	}
}

icon_hidden( player )
{
	return !isdefined( player.friendlyIcon ) || player.friendlyIcon.alpha == 0;
}

// Price's eyes, the passenger's body's eyes and the passenger's own eyes in the boat's frame
// (forward, right, up), with the poses.
boat_poses_text( other )
{
	text = "Price " + level.price.a.boat_pose + " eye " + boat_local( level.price GetTagOrigin( "tag_eye" ) ) + bones_text( level.price );
	if ( isdefined( level.mw2coop_boat_body ) )
		text += ", body " + level.mw2coop_boat_body.mw2coop_pose + " eye " + boat_local( level.mw2coop_boat_body GetTagOrigin( "tag_eye" ) ) + bones_text( level.mw2coop_boat_body );
	text += ", passenger eye " + boat_local( other GetEye() );
	// where the guns and the view point, degrees from the bow, to the left positive
	text += "; yaw: Price gun " + gun_yaw( level.price ) + " (his aim " + int( level.price.a.boatAimYaw ) + ")";
	if ( isdefined( level.mw2coop_boat_body ) )
		text += ", body gun " + gun_yaw( level.mw2coop_boat_body );
	return text + ", passenger view " + view_yaw( other );
}

view_yaw( player )
{
	return int( AngleClamp180( player GetPlayerAngles()[ 1 ] - level.players_boat.angles[ 1 ] ) );
}

gun_follows_view( player )
{
	return isdefined( level.mw2coop_boat_body ) && abs( AngleClamp180( gun_yaw( level.mw2coop_boat_body ) - view_yaw( player ) ) ) < 8;
}

gun_yaw( ent )
{
	return int( AngleClamp180( ent GetTagAngles( "tag_flash" )[ 1 ] - level.players_boat.angles[ 1 ] ) );
}

// Bones every human model has: root, pelvis, head.
bones_text( ent )
{
	return " root " + boat_local( ent GetTagOrigin( "tag_origin" ) ) + " pelvis " + boat_local( ent GetTagOrigin( "j_mainroot" ) ) + " head " + boat_local( ent GetTagOrigin( "j_head" ) );
}

boat_local( pos )
{
	boat = level.players_boat;
	d = pos - boat.origin;
	return int( VectorDot( d, AnglesToForward( boat.angles ) ) ) + " " + int( VectorDot( d, AnglesToRight( boat.angles ) ) ) + " " + int( VectorDot( d, AnglesToUp( boat.angles ) ) );
}

// Price's eyes from the boat's long axis, to the right positive.
price_side()
{
	boat = level.players_boat;
	return VectorDot( level.price GetTagOrigin( "tag_eye" ) - boat.origin, AnglesToRight( boat.angles ) );
}

// The passenger's eyes on the other side of the boat's long axis from Price's (his "left" pose
// lies on the right tube: QA boat measured it).
across_from_price( other )
{
	boat = level.players_boat;
	side = VectorDot( other GetEye() - boat.origin, AnglesToRight( boat.angles ) );
	return side * price_side() < 0 && abs( side ) > 8;
}

// The player's distance from the boat's long axis, to the right positive.
boat_side( player )
{
	boat = level.players_boat;
	return VectorDot( player.origin - boat.origin, AnglesToRight( boat.angles ) );
}

stumbler_runs_watch()
{
	level waittill( "run_shep_run" );
	level.mw2coop_qa_stumbler_runs = true;
}

// A floor spot at one of the distances from center where the stumbler is (or is not) in
// shep_trace_passed()'s sight.
sight_spot( stumbler, center, dists, want_sight )
{
	foreach ( dist in strtok( dists, " " ) )
	{
		for ( yaw = 0; yaw < 360; yaw += 20 )
		{
			at = center + AnglesToForward( ( 0, yaw, 0 ) ) * int( dist );
			floor = PlayerPhysicsTrace( at + ( 0, 0, 150 ), at - ( 0, 0, 300 ) );
			if ( abs( floor[ 2 ] - center[ 2 ] ) > 150 )
				continue;
			if ( sees_stumbler( floor, stumbler ) == want_sight )
				return floor;
		}
	}
	return undefined;
}

sees_stumbler( origin, stumbler )
{
	for ( offset = 0; offset <= 48; offset += 16 )
	{
		trace = BulletTrace( origin + ( 0, 0, 32 ), stumbler GetEye() + ( 0, 0, offset ), false, undefined );
		if ( trace[ "fraction" ] >= 1 || trace[ "surfacetype" ] == "none" )
			return true;
	}
	return false;
}

shepherd_hidden()
{
	return isdefined( level.mw2coop_shepherd_hidden ) && level.mw2coop_shepherd_hidden;
}

// One strike attempt: in front of Shepherd, looking at his eyes, melee (0.25 s).
knife_strike_try( striker )
{
	fwd = AnglesToForward( ( 0, level.shepherd.angles[ 1 ], 0 ) );
	at = level.shepherd.origin + fwd * 70;
	if ( Distance( striker.origin, at ) > 30 )
		striker SetOrigin( PlayerPhysicsTrace( at + ( 0, 0, 40 ), at - ( 0, 0, 80 ) ) );
	striker SetPlayerAngles( VectorToAngles( level.shepherd GetTagOrigin( "tag_eye" ) - striker GetEye() ) );
	striker.mw2coop_qa_melee = true;
	wait 0.1;
	striker.mw2coop_qa_melee = undefined;
	wait 0.15;
}

knife_out_watch()
{
	level waittill( "knife_pulled_out" );
	level.mw2coop_qa_knife_out = true;
}

// Every 10 s: how far the other player's view is from the action and whether he sees it.
knife_watch_report( other )
{
	level endon( "mw2coop_knife_witness_up" );
	for ( ;; )
	{
		wait 10;
		if ( !isdefined( level.shepherd ) )
			continue;
		eye = other GetEye();
		focus = level.shepherd GetTagOrigin( "tag_eye" );
		sees = BulletTracePassed( eye, focus, false, undefined );
		dot = VectorDot( AnglesToForward( other GetPlayerAngles() ), VectorNormalize( focus - eye ) );
		report( "witness: awake " + other.mw2coop_awake + ", " + int( Distance( eye, focus ) ) + " units from Shepherd, line " + sees + ", facing " + int( dot * 100 ) + "%" );
	}
}

// QA body: put player 2 in front of player 1, facing him (the bodies come from _mw2coop);
// screenshots show how each sees the other.
body()
{
	wait 3;
	angles = level.player GetPlayerAngles();
	yaw = angles[ 1 ];
	forward = AnglesToForward( ( 0, yaw, 0 ) );
	level.player2 SetOrigin( level.player.origin + forward * 120 );
	level.player SetPlayerAngles( ( 0, yaw, 0 ) );
	level.player2 SetPlayerAngles( ( 0, yaw + 180, 0 ) );
	report( "body: models set, player 2 in front of player 1" );
	report( "body: weapons " + level.player GetCurrentWeapon() + " / " + level.player2 GetCurrentWeapon() );
	wait 15;
	level.player2 SetStance( "crouch" );
	report( "body: player 2 crouches" );
	wait 25;
	report( "done" );
}


// Endgame boat chase (af_chase, default start). Names no chase script (not in other zones): the
// bot boards through the jetty trigger and drives with level.mw2coop_qa_boat_autopilot.
boat()
{
	check( level.players.size == 2 && isdefined( level.player2 ), "two players (" + level.players.size + ")" );
	driver = level.player;
	if ( GetDvar( "mw2coop_qa" ) == "boat2" )
		driver = level.player2;
	other = maps\_mw2coop::other_player( driver );
	trigger = GetEnt( "boat_mount", "targetname" );
	check( isdefined( trigger ), "the jetty's boat trigger" );
	wait 2;
	driver SetOrigin( PlayerPhysicsTrace( trigger.origin + ( 0, 0, 40 ), trigger.origin - ( 0, 0, 200 ) ) );
	for ( t = 0; t < 3 && !flag( "player_on_boat" ); t += 0.25 )
		wait 0.25;
	if ( !flag( "player_on_boat" ) )
	{
		report( "QA sets off the boat trigger (standing in it did not)" );
		trigger notify( "trigger", driver );
	}
	for ( t = 0; t < 10 && !flag( "player_on_boat" ); t += 0.25 )
		wait 0.25;
	check( flag( "player_on_boat" ), "on the boat (" + t + " s)" );
	wait 1;
	check( level.mw2coop_lead == driver, "the boarding player drives" );
	check( isdefined( driver.drivingVehicle ) && driver.drivingVehicle == level.players_boat, "and is at the wheel" );
	d = Distance( other.origin, level.players_boat.origin );
	// he is seated once Price has settled in the boat (up to 5 s)
	for ( t = 0; t < 7 && !isdefined( level.mw2coop_boat_seat ); t += 0.25 )
		wait 0.25;
	d = Distance( other.origin, level.players_boat.origin );
	check( other IsLinked() && d < 150, "the other player rides in the boat (" + int( d ) + " units from it, seated after " + t + " s)" );
	if ( isdefined( level.mw2coop_qa_boat_autopilot ) )
		level.players_boat thread [[ level.mw2coop_qa_boat_autopilot ]]( 60 );
	// the body's eyes (forward right up in the boat's frame) every 0.5 s
	text = "body samples:";
	for ( t = 0; t < 3; t += 0.5 )
	{
		if ( isdefined( level.mw2coop_boat_body ) )
			text += " " + boat_local( level.mw2coop_boat_body GetTagOrigin( "tag_eye" ) );
		else
			text += " -";
		wait 0.5;
	}
	report( text );
	report( "poses: " + boat_poses_text( other ) );
	check( gun_follows_view( other ), "his body's gun points where he looks (gun " + gun_yaw( level.mw2coop_boat_body ) + ", view " + view_yaw( other ) + ")" );
	check( abs( view_yaw( other ) ) < 20, "he starts looking ahead over the bow (" + view_yaw( other ) + ")" );	// (the view eases toward the arc's centre, -15, once he is linked)
	check( across_from_price( other ), "the passenger lies across from Price (" + int( boat_side( other ) ) + ", Price " + int( price_side() ) + ")" );
	// where is the driver (the other player's body) from the passenger's seat? In degrees of the bow (left positive) and units, and in the boat's own frame
	boat = level.players_boat;
	eye = other GetEye();
	foreach ( who in level.players )
	{
		d = who.origin - eye;
		fwd = VectorDot( d, AnglesToForward( boat.angles ) );
		right = VectorDot( d, AnglesToRight( boat.angles ) );
		ang = VectorToAngles( d );
		report( "where: " + player_name( who ) + " is " + int( fwd ) + " ahead, " + int( right ) + " right, " + int( d[ 2 ] ) + " up of the passenger's eye: " + int( AngleClamp180( ang[ 1 ] - boat.angles[ 1 ] ) ) + " degrees of the bow (left positive), " + int( Distance( who.origin, eye ) ) + " units" );
	}
	// he looks as far right, then left, as he can (screenshots: his body's gun follows)
	foreach ( turn in strtok( "-45 45", " " ) )
	{
		report( "passenger looks " + turn );
		for ( t = 0; t < 3; t += 0.05 )
		{
			other SetPlayerAngles( ( 0, level.players_boat.angles[ 1 ] - 36 + int( turn ), 0 ) );
			wait 0.05;
		}
	}
	check( gun_follows_view( other ), "after looking about, the gun still follows (gun " + gun_yaw( level.mw2coop_boat_body ) + ", view " + view_yaw( other ) + ")" );
	// his gun is an M4A1 with the grenade launcher, not the Uzi (owner, 2026-11-10)
	check( other HasWeapon( "m4_grenadier" ) && !other HasWeapon( "uzi" ), "the passenger has the M4A1 grenadier and no Uzi (" + other GetCurrentWeapon() + ")" );
	check( other GetCurrentWeapon() == "m4_grenadier", "the passenger holds the M4A1 grenadier (" + other GetCurrentWeapon() + ")" );
	// his camera covers a half circle turned to -15 degrees of the bow in the right pose (owner, 2026-11-10): -105 to +75, clear of the driver behind him (-173); asked for -100 it gets there, asked for +100 it stops at about +75
	foreach ( want in strtok( "-100 100", " " ) )
	{
		for ( t = 0; t < 1.5; t += 0.05 )
		{
			other SetPlayerAngles( ( 0, level.players_boat.angles[ 1 ] + int( want ), 0 ) );
			wait 0.05;
		}
		if ( int( want ) < 0 )
			check( abs( view_yaw( other ) - int( want ) ) < 10, "his camera reaches " + want + " degrees of the bow (" + view_yaw( other ) + ")" );
		else
			check( view_yaw( other ) > 65 && view_yaw( other ) < 85, "his camera stops at about +75 degrees of the bow when asked for " + want + " (" + view_yaw( other ) + ")" );
	}
	other SetPlayerAngles( ( 0, level.players_boat.angles[ 1 ], 0 ) );
	wait 0.5;
	// the level's triggers that would turn Price over (af_chase_code::price_position_switch())
	// leave him on his tube in co-op: set them all off
	switches = GetEntArray( "price_position_switch", "targetname" );
	foreach ( s in switches )
		s notify( "trigger", level.mw2coop_lead );
	wait 3;
	report( "poses: " + boat_poses_text( other ) );
	check( switches.size > 0 && level.price.a.boat_pose == "left" && !isdefined( level.price.scripted_boat_pose ), "Price keeps his tube through the level's " + switches.size + " position triggers" );
	check( across_from_price( other ), "the passenger still lies across from him (" + int( boat_side( other ) ) + ", Price " + int( price_side() ) + ")" );
	price_turns = 0;
	// 30 s: the QA autopilot is slower than a player, and by 35 to 45 s Shepherd's boat gets
	// away and the level fails the mission as it would for a player that slow
	for ( t = 0; t < 30; t += 10 )
	{
		// Price's root and eyes against the tag, and the body's eyes: what moves while riding
		for ( s = 0; s < 10; s += 2 )
		{
			wait 2;
			if ( level.price.a.boat_pose != "left" )
				price_turns++;
			if ( isdefined( level.mw2coop_boat_body ) )
				report( "drift: Price " + level.price.a.boat_pose + " root " + boat_local( level.price.origin ) + " eye " + boat_local( level.price GetTagOrigin( "tag_eye" ) ) + ", body " + level.mw2coop_boat_body.mw2coop_pose + " eye " + boat_local( level.mw2coop_boat_body GetTagOrigin( "tag_eye" ) ) );
		}
		report( "ride " + ( t + 10 ) + " s: boat speed " + int( level.players_boat Vehicle_GetSpeed() ) + ", passenger " + int( Distance( other.origin, level.players_boat.origin ) ) + " units from it, linked " + other IsLinked() + ", health " + other.health + ", enemy boat " + int( Distance( level.players_boat.origin, level.enemy_boat.origin ) ) );
	}
	check( IsAlive( other ) && other IsLinked(), "the passenger rode along" );
	check( price_turns == 0, "Price stayed on his tube the whole ride (" + price_turns + " samples off it)" );
	report( "done" );
}

// --------------------------------------------------------------------------------------------
// Endgame's checkpoints (issue #16; maps\_mw2coop_checkpoints af_chase). The bot does not drive (the QA autopilot made the game run at a tenth of its
// speed after a minute): the players' boat is put at each start point's place, a little past it, and the level's trigger for that checkpoint is set
// off if the boat did not touch it (their places against the start points: map data, in the table's notes). Run 1 (default start): no checkpoint
// with the level's start, boarding, the cave exit, the middle of the lake, then the mission is failed; run 2 (the restart there): the boat and
// both players are back, no other checkpoint with the restart, the rapids, the river below them, the boarding scene, failed again; run 3: the
// restart at the end of the chase. Phases: "boatcp:start", "boatcp:mid", "boatcp:end".
boatcp_order()
{
	return strtok( "boatdrive_begin boatdrive_lake boatdrive_lake_mid boatdrive_rapids boatdrive_below_rapids boatdrive_end", " " );
}

boatcp()
{
	check( level.players.size == 2 && isdefined( level.player2 ), "two players (" + level.players.size + ")" );
	order = boatcp_order();
	phase = GetDvar( "mw2coop_qa_phase" );
	thread boatcp_no_escape();
	if ( phase == "boatcp:mid" || phase == "boatcp:end" )
	{
		want = "boatdrive_lake_mid";
		if ( phase == "boatcp:end" )
			want = "boatdrive_end";
		check( level.start_point == want, "the failure restarted at " + want + " (start point " + level.start_point + ")" );
		for ( t = 0; t < 10 && !flag( "player_on_boat" ); t += 0.25 )
			wait 0.25;
		check( flag( "player_on_boat" ), "on the boat after the restart" );
		for ( t = 0; t < 7 && !isdefined( level.mw2coop_boat_seat ); t += 0.25 )
			wait 0.25;
		other = maps\_mw2coop::other_player( level.mw2coop_lead );
		check( level.player IsLinked() && level.player2 IsLinked() && Distance( other.origin, level.players_boat.origin ) < 150, "both players in the boat (" + int( Distance( other.origin, level.players_boat.origin ) ) + " units from it)" );
		wait 4;
		check( GetDvar( "start" ) == want, "no other checkpoint with the restart (start '" + short_start() + "')" );
		if ( phase == "boatcp:end" )
		{
			SetDvar( "mw2coop_qa_phase", "done" );
			report( "done" );
			return;
		}
		ok = boatcp_step( "boatdrive_rapids", "rapids_start_position", "rapids_trigger", undefined );
		if ( ok )
			ok = boatcp_step( "boatdrive_below_rapids", "below_rapids_start_position", "on_river_trigger", undefined );
		if ( ok )
			ok = boatcp_step( "boatdrive_end", "end_start_position", undefined, "player_in_sight_of_boarding" );
		if ( !ok )
		{
			SetDvar( "mw2coop_qa_phase", "done" );
			report( "done" );
			return;
		}
		boatcp_fail( "boatcp:end", "at the end of the chase" );
		return;
	}
	if ( phase != "" )
	{
		report( "boatcp: the level restarted at " + level.start_point + " in phase " + phase + " (the level's own failure): stopping" );
		SetDvar( "mw2coop_qa_phase", "done" );
		report( "done" );
		return;
	}
	SetDvar( "mw2coop_qa_phase", "boatcp:start" );
	wait 4;	// past the first 3 s, where no section is a checkpoint
	check( !is_in_array( order, GetDvar( "start" ) ), "no checkpoint with the level's start (start '" + short_start() + "')" );
	driver = level.player;
	trigger = GetEnt( "boat_mount", "targetname" );
	driver SetOrigin( PlayerPhysicsTrace( trigger.origin + ( 0, 0, 40 ), trigger.origin - ( 0, 0, 200 ) ) );
	for ( t = 0; t < 3 && !flag( "player_on_boat" ); t += 0.25 )
		wait 0.25;
	if ( !flag( "player_on_boat" ) )
		trigger notify( "trigger", driver );
	for ( t = 0; t < 10 && !flag( "player_on_boat" ); t += 0.25 )
		wait 0.25;
	check( flag( "player_on_boat" ), "on the boat" );
	wait 0.5;
	check( GetDvar( "start" ) == "boatdrive_begin", "checkpoint boatdrive_begin on boarding (start '" + short_start() + "')" );
	for ( t = 0; t < 7 && !isdefined( level.mw2coop_boat_seat ); t += 0.25 )
		wait 0.25;
	wait 2;
	ok = boatcp_step( "boatdrive_lake", "lake_start_pose", "end_caves_trigger", undefined );
	if ( ok )
		ok = boatcp_step( "boatdrive_lake_mid", "lake_mid_start_pose", undefined, undefined );
	if ( !ok )
	{
		SetDvar( "mw2coop_qa_phase", "done" );
		report( "done" );
		return;
	}
	boatcp_fail( "boatcp:mid", "in the middle of the lake" );
}

// The players' boat to 300 units past the start point's place (struct pose), along its heading; then, if the checkpoint has not come within
// 2 s, the level's trigger is set off (by targetname, or the flag trigger of flag_name). True when the checkpoint came.
boatcp_step( name, pose, trigger_name, flag_name )
{
	spot = getstruct( pose, "targetname" );
	if ( !isdefined( spot ) )
	{
		check( false, "the place " + pose + " of " + name );
		return false;
	}
	yaw = spot.angles[ 1 ];
	to = spot.origin + AnglesToForward( ( 0, yaw, 0 ) ) * 300;
	level.players_boat Vehicle_Teleport( to, ( 0, yaw, 0 ) );
	report( "boat put 300 units past " + pose + " at " + vec_int( to ) + " (game time " + GetTime() + ")" );
	for ( t = 0; t < 2 && GetDvar( "start" ) != name; t += 0.25 )
		wait 0.25;
	how = "by the place";
	if ( GetDvar( "start" ) != name )
	{
		trigger = undefined;
		if ( isdefined( trigger_name ) )
			trigger = GetEnt( trigger_name, "targetname" );
		else if ( isdefined( flag_name ) )
			foreach ( candidate in GetEntArray( "trigger_multiple_flag_set", "classname" ) )
				if ( isdefined( candidate.script_flag ) && candidate.script_flag == flag_name )
					trigger = candidate;
		if ( isdefined( trigger ) )
		{
			how = "by its trigger, set off";
			trigger notify( "trigger", level.mw2coop_lead );
		}
		for ( t = 0; t < 3 && GetDvar( "start" ) != name; t += 0.25 )
			wait 0.25;
	}
	ok = GetDvar( "start" ) == name;
	check( ok, "checkpoint " + name + " " + how + " (boat at " + vec_int( level.players_boat.origin ) + ", start '" + short_start() + "')" );
	return ok;
}

boatcp_fail( next_phase, where )
{
	SetDvar( "mw2coop_qa_phase", next_phase );
	wait 2;
	report( "failing the mission " + where );
	level thread maps\_mw2coop::mission_failed();
	wait 40;
	report( "FAIL no restart 40 s after the failure" );
}

// Shepherd's boat drops a crumb on its path every so often (af_chase_code::enemy_chase_boat_breadcrumb); the mission fails once the oldest crumb
// the players' boat has not passed is older than the limit (10 s). Crumbs older than 6 s are dropped here, so the list stays short and the bot's
// slow boat never trails too far. (Raising the limit instead let the list grow without end: the level walks it every frame, and after two
// minutes a frame took 0.9 s and both copies froze.)
boatcp_no_escape()
{
	level endon( "mw2coop_level_end" );
	for ( ;; )
	{
		if ( isdefined( level.breadcrumb ) && level.breadcrumb.size )
		{
			keep = [];
			foreach ( crumb in level.breadcrumb )
				if ( GetTime() - crumb.spawn_time < 6000 )
					keep[ keep.size ] = crumb;
			level.breadcrumb = keep;
		}
		wait 0.25;
	}
}

// cpwalk: (a level with flags or places in maps\_mw2coop_checkpoints, any start) no checkpoint with the level's start; then, in the order of
// their sections, player 1 is put on each flag's trigger (a flag set only by a level script is set) or just past each place, and its start
// point must become the checkpoint. It checks the table and maps\_mw2coop::section_reached, not the level's own way there.
cpwalk()
{
	check( level.players.size == 2 && isdefined( level.player2 ), "two players (" + level.players.size + ")" );
	wait 4;
	// a level begun at a checkpoint takes none in its first 8 s (maps\_mw2coop::just_started_at_checkpoint)
	while ( isdefined( level.start_point ) && level.start_point != "default" && GetTime() < 8500 )
		wait 0.25;
	// the table's flags and places of this level after the start point it began at, in the order of their sections
	entries = [];
	foreach ( p in maps\_mw2coop_checkpoints::table( level.script ) )
		entries[ entries.size ] = cpwalk_entry( "flag", p[ 0 ], p[ 1 ] );
	foreach ( p in maps\_mw2coop_checkpoints::places( level.script ) )
		entries[ entries.size ] = cpwalk_entry( "place", p[ 0 ], p[ 1 ] );
	names = [];
	foreach ( e in entries )
		names[ names.size ] = e.start;
	check( !is_in_array( names, GetDvar( "start" ) ), "no checkpoint with the level's start (start '" + short_start() + "')" );
	for ( i = 0; i < entries.size; i++ )
		for ( j = i + 1; j < entries.size; j++ )
			if ( entries[ j ].index < entries[ i ].index )
			{
				e = entries[ i ];
				entries[ i ] = entries[ j ];
				entries[ j ] = e;
			}
	floor_index = maps\_mw2coop::start_section_index();
	foreach ( e in entries )
		if ( e.index < 0 )
			check( false, "the table's start point " + e.start + " (" + e.kind + " " + e.key + ") is one of the level's start points" );
	triggers = array_combine( GetEntArray( "trigger_multiple_flag_set_touching", "classname" ), GetEntArray( "trigger_multiple_flag_set", "classname" ) );
	foreach ( e in entries )
	{
		if ( e.index <= floor_index )
			continue;
		// a scene holding player 1 (a vehicle, a rig) ends first
		for ( t = 0; t < 90 && level.player IsLinked(); t += 0.5 )
			wait 0.5;
		label = e.kind + " " + e.key;
		if ( e.kind == "place" )
		{
			spot = maps\_mw2coop_checkpoints::place_of( e.key );
			if ( !isdefined( spot ) )
			{
				check( false, "the place " + e.key + " of " + e.start + " is in the level" );
				continue;
			}
			yaw = 0;
			if ( isdefined( spot.angles ) )
				yaw = spot.angles[ 1 ];
			to = spot.origin + AnglesToForward( ( 0, yaw, 0 ) ) * 120;
			level.player SetOrigin( PlayerPhysicsTrace( to + ( 0, 0, 48 ), to - ( 0, 0, 256 ) ) );
			label = "player 1 just past the place " + e.key;
		}
		else
		{
			trigger = undefined;
			foreach ( candidate in triggers )
				if ( isdefined( candidate.script_flag ) && candidate.script_flag == e.key )
					trigger = candidate;
			if ( isdefined( trigger ) && isdefined( trigger.spawnflags ) && ( trigger.spawnflags & 8 ) )
			{
				// a trigger only AI set off (spawnflags NOTPLAYER: Wolverines!' president in the meat locker): the flag itself
				flag_set( e.key );
				label = "flag " + e.key + " set (its trigger is for AI only)";
			}
			else if ( isdefined( trigger ) )
			{
				level.player SetOrigin( PlayerPhysicsTrace( trigger.origin + ( 0, 0, 48 ), trigger.origin - ( 0, 0, 256 ) ) );
				label = "player 1 on the trigger of " + e.key;
			}
			else
			{
				// set by the level's own script (Endgame's boarding, No Russian's end of the massacre): the flag itself
				flag_set( e.key );
				label = "flag " + e.key + " set (no map trigger sets it)";
			}
		}
		for ( t = 0; t < 5 && GetDvar( "start" ) != e.start; t += 0.25 )
			wait 0.25;
		// the teleport may land outside a trigger's volume (only its centre is known): then the flag itself, so the table is still checked
		if ( GetDvar( "start" ) != e.start && e.kind == "flag" && !flag( e.key ) )
		{
			report( "player 1 at " + vec_int( level.player.origin ) + " did not set off " + e.key + " (the teleport missed its trigger?): the flag is set" );
			flag_set( e.key );
			label = "flag " + e.key + " set after the missed trigger";
			for ( t = 0; t < 5 && GetDvar( "start" ) != e.start; t += 0.25 )
				wait 0.25;
		}
		check( GetDvar( "start" ) == e.start, label + " at " + vec_int( level.player.origin ) + ": checkpoint " + e.start + " (start '" + short_start() + "')" );
		wait 1;
	}
	report( "done" );
}

cpwalk_entry( kind, key, start )
{
	e = SpawnStruct();
	e.kind = kind;
	e.key = key;
	e.start = ToLower( start );	// maps\_utility::add_start lowers the names
	e.index = maps\_mw2coop::section_index( e.start );
	if ( !isdefined( e.index ) )
		e.index = -1;
	return e;
}

// the start dvar, short (at the default start it holds the list of all start points)
short_start()
{
	s = GetDvar( "start" );
	if ( s.size > 40 )
		return GetSubStr( s, 0, 40 ) + "...";
	return s;
}

// The level's loadout (maps\_loadout): player 2 has player 1's weapons and holds the same one.
loadout()
{
	check( level.players.size == 2 && isdefined( level.player2 ), "two players (" + level.players.size + ")" );
	w = GetDvarInt( "mw2coop_qa_wait" ); // mw2coop_qa_wait <s>: check later (after a landing, a scene)
	if ( w <= 0 )
		w = 5;
	wait w;
	kit_checks();
	report( "done" );
}

// What the level gave player 1, player 2 has too: weapons (maps\_mw2coop::sync_weapons), the one
// in his hands, the second grenade class (src/mirror.c), the enemies' threat bias group (mirror)
// and threat bias and accuracy against him (maps\_mw2coop::players_threatbias,
// players_attackeraccuracy).
kit_checks()
{
	p1 = level.player;
	p2 = level.player2;
	a = p1 GetCurrentWeapon();
	b = p2 GetCurrentWeapon();
	report( "player 1: " + a + " | " + weapons_text( p1 ) );
	report( "player 2: " + b + " | " + weapons_text( p2 ) );
	check( a == b, "both hold " + a + " (player 2: " + b + ")" );
	missing = "";
	foreach ( weapon in p1 GetWeaponsListAll() )
		if ( !p2 HasWeapon( weapon ) )
			missing += weapon + " ";
	check( missing == "", "player 2 has all of player 1's weapons (missing: " + missing + ")" );
	c1 = p1 GetOffhandSecondaryClass();
	c2 = p2 GetOffhandSecondaryClass();
	check( c1 == c2, "same second grenade class " + c1 + " (player 2: " + c2 + ")" );
	g1 = bias_group( p1 );
	g2 = bias_group( p2 );
	check( g1 == g2, "same threat bias group '" + g1 + "' (player 2: '" + g2 + "')" );
	report( "threatbias " + p1.threatbias + "/" + p2.threatbias + ", attackeraccuracy " + p1.attackeraccuracy + "/" + p2.attackeraccuracy );
}

// --------------------------------------------------------------------------------------------
// S.S.D.D. (trainer): each player takes his own weapons (maps\_mw2coop_trainer). Start point ""
// tests the rifle, "frag" the grenade box, "pit" the pistol. Each player stands by the weapon,
// looks at it and presses Use on his own machine (press()), as the owner did by hand.
pickups()
{
	check( level.players.size == 2 && isdefined( level.player2 ), "two players (" + level.players.size + ")" );
	wait_intro();	// player 1 can take nothing before
	start = GetDvar( "start" );
	if ( start == "frag" )
	{
		level.mw2coop_no_follow = true;
		pickup_frags();
		return;
	}
	if ( start == "pit" )
	{
		level.mw2coop_no_follow = true;
		pickup_table( "pickup_sidearm", "deserteagle", "pistol" );
		report( "done" );
		return;
	}
	flag_wait( "firing_range_initialized" );
	wait 1;
	check( !level.player HasWeapon( "m4_grunt" ) && !level.player2 HasWeapon( "m4_grunt" ), "no rifles at the range yet" );
	// both inside the sandbag bay where COD-MOD puts its players, player 1 on the left
	// (maps\_mw2coop_trainer::range_place_1)
	start_ent = GetEnt( "player_start_range", "targetname" );
	right = AnglesToRight( ( 0, start_ent.angles[ 1 ], 0 ) );
	side1 = int( VectorDot( level.player.origin - start_ent.origin, right ) );
	side2 = int( VectorDot( level.player2.origin - start_ent.origin, right ) );
	check( side1 < side2 - 30, "player 1 left of player 2 at the range (" + side1 + ", " + side2 + " from the start, to the right)" );
	check( Distance2D( level.player2.origin, start_ent.origin ) < 8 && Distance2D( level.player.origin, ( -2515.2, 5190.2, 0 ) ) < 8, "both on their places in the sandbag bay" );
	foreach ( player in level.players )
	{
		check( !qa_stuck( player ), player_name( player ) + " is not stuck in anything solid at his place" );
		check( qa_in_bay( player, start_ent ), player_name( player ) + " is inside the sandbags (" + bay_text( player, start_ent ) + ")" );
	}
	check( abs( level.player.origin[ 2 ] - level.player2.origin[ 2 ] ) < 4, "both on the floor (" + int( level.player.origin[ 2 ] ) + ", " + int( level.player2.origin[ 2 ] ) + ")" );
	// player 2 was brought along to the range: he must stand where he can walk away
	report( "at the range: player 1 " + vec_int( level.player.origin ) + ", player 2 " + vec_int( level.player2.origin ) + ", " + int( Distance( level.player.origin, level.player2.origin ) ) + " apart" );
	level.mw2coop_no_follow = true;
	flag_wait( "foley_anim_pickup_weapon" );
	wait 2;
	pickup_table( "pickup_rifle", "m4_grunt", "rifle" );
	for ( t = 0; t < 90 && !flag( "player_picked_up_rifle" ); t += 0.5 )
		wait 0.5;
	check( flag( "player_picked_up_rifle" ), "the drill goes on once both have rifles (" + t + " s)" );
	report( "done" );
}

// Player 2 takes the weapon first, then player 1 the one that lies there again.
pickup_table( name, weapon, what )
{
	for ( t = 0; t < 60 && !pickup_ready( name ); t += 0.5 )
		wait 0.5;
	check( pickup_ready( name ), "a " + what + " on the table" );
	if ( !pickup_ready( name ) )
		return;
	first = level.mw2coop_pickups[ name ];
	qa_use_take( level.player2, first, weapon );
	check( level.player2 HasWeapon( weapon ) && !level.player HasWeapon( weapon ), "player 2 takes the " + what + ", player 1 has none (" + weapons_text( level.player ) + ")" );
	wait 1;
	check( pickup_ready( name ) && level.mw2coop_pickups[ name ].model != "" && level.mw2coop_pickups[ name ].model != "_obj", "another " + what + " lies on the table (" + pickup_model( name ) + ")" );
	if ( pickup_ready( name ) )
		qa_use_take( level.player, level.mw2coop_pickups[ name ], weapon );
	check( level.player HasWeapon( weapon ) && level.player2 HasWeapon( weapon ), "player 1 takes his own " + what );
	wait 1;
	check( !pickup_ready( name ), "no third " + what + " on the table" );
}

pickup_model( name )
{
	if ( !pickup_ready( name ) )
		return "none";
	return level.mw2coop_pickups[ name ].model;
}

pickup_ready( name )
{
	return isdefined( level.mw2coop_pickups ) && isdefined( level.mw2coop_pickups[ name ] );
}

// The level intro holds both players (maps\_introscreen patch; before, player 2 walked and took
// a pistol under S.S.D.D.'s title): both press Forward under the title and stay, then both walk.
introfreeze()
{
	check( level.players.size == 2 && isdefined( level.player2 ), "two players (" + level.players.size + ")" );
	level.mw2coop_no_follow = true;
	level.mw2coop_qa_intro_over = flag_exist( "introscreen_complete" ) && flag( "introscreen_complete" );
	level thread qa_note_intro_over();
	from = [];
	foreach ( n, player in level.players )
	{
		from[ n ] = player.origin;
		press( player, "forward", 1000 );
	}
	wait 1;
	over = level.mw2coop_qa_intro_over;
	foreach ( n, player in level.players )
	{
		// a player riding something (Takedown's car) is carried along and cannot walk anyway
		if ( player IsLinked() )
			report( player_name( player ) + " rides (linked) under the title: not checked" );
		else
			check( !over && Distance2D( player.origin, from[ n ] ) < 2, player_name( player ) + " stays still on a Forward press under the intro title (" + int( Distance2D( player.origin, from[ n ] ) ) + " units; intro over: " + over + ")" );
	}
	wait_intro();
	wait 1;
	foreach ( player in level.players )
	{
		if ( player IsLinked() )
		{
			report( player_name( player ) + " rides (linked) after the intro: not checked" );
			continue;
		}
		walked = qa_walk( player );	// before the text: GSC evaluates arguments last to first
		check( walked, player_name( player ) + " walks after the intro (" + level.mw2coop_qa_walk + ")" );
	}
	report( "done" );
}

qa_note_intro_over()
{
	level waittill( "introscreen_complete" );
	level.mw2coop_qa_intro_over = true;
}

// Turns him the way with the most free floor (up to 100 units) and presses Forward for 1 s; true
// when he walked at least half of that room (40 units at most). level.mw2coop_qa_walk says how far.
qa_walk( player )
{
	yaw = player GetPlayerAngles()[ 1 ];
	room = 0;
	best = yaw;
	for ( turn = 0; turn < 360; turn += 45 )
	{
		from = player.origin + ( 0, 0, 8 );
		dir = AnglesToForward( ( 0, yaw + turn, 0 ) );
		free = Distance( from, PlayerPhysicsTrace( from, from + dir * 100 ) );
		// only as far as there is floor: Cliffhanger's cave ledge is open towards the drop, and the
		// bot walked off it where a player walks sideways along the ice (owner, 2026-10-22)
		for ( d = 25; d <= free; d += 25 )
		{
			at = from + dir * d;
			if ( at[ 2 ] - PlayerPhysicsTrace( at, at - ( 0, 0, 64 ) )[ 2 ] > 48 )
			{
				free = d - 25;
				break;
			}
		}
		if ( free > room + 1 )
		{
			room = free;
			best = yaw + turn;
		}
	}
	player SetPlayerAngles( ( 0, best, 0 ) );
	wait 0.2;
	start = player.origin;
	press( player, "forward", 1000 );
	wait 1.5;
	moved = Distance2D( player.origin, start );
	level.mw2coop_qa_walk = int( moved ) + " of " + int( room ) + " free units";
	// 15: Cliffhanger's ledge holds the player to a 0.35 walk (maps\_climb::climb_wall), 17-39 units
	return moved > min( 15, room * 0.5 );
}

// --------------------------------------------------------------------------------------------
// A level's sections, one after another (owner, 2026-10-22: on the levels without big scenes run
// the players through the trigger points and look at what they see). Each start point is a
// section; the next one is reached by a restart at it (as a checkpoint restart is).
tour()
{
	starts = tour_starts();
	at = -1;	// the default start comes before the first named one
	foreach ( n, name in starts )
		if ( name == level.start_point )
			at = n;
	report( "tour " + level.script + ": section " + level.start_point + " (" + ( at + 2 ) + " of " + ( starts.size + 1 ) + ")" );
	// back at the same section: the level failed the mission in it (a stealth section that sees
	// the bot, Just Like Old Times barracks) and restarted it; go on with the next one
	if ( GetDvar( "mw2coop_qa_phase" ) == "tour:" + level.start_point )
		report( "the mission failed in section " + level.start_point + " (the level's own fail, e.g. spotted): skipped" );
	else
	{
		SetDvar( "mw2coop_qa_phase", "tour:" + level.start_point );
		tour_section();
	}
	if ( at + 1 < starts.size )
	{
		SetDvar( "start", starts[ at + 1 ] );
		report( "next section " + starts[ at + 1 ] );
		maps\_mw2coop::request( "restart" );
		return;
	}
	SetDvar( "mw2coop_qa_phase", "done" );
	report( "done" );
}

// cptour: (any map, default start) for every start point of the level: the section starts, the mission is
// failed after a few seconds, and the level must start again at the same section with both players up
// and together; then the next section. The restart from a checkpoint (the section the level reached)
// is this same path.
cptour()
{
	starts = tour_starts();
	at = -1;	// the default start comes before the first named one
	foreach ( n, name in starts )
		if ( name == level.start_point )
			at = n;
	here = level.start_point;
	phase = GetDvar( "mw2coop_qa_phase" );
	// the section the mission was failed in: the level may restart at its own checkpoint for it (Favela's
	// "torture" restarts at "favela"), which is the next section to count from, not a new section to fail in
	was = here;
	if ( GetSubStr( phase, 0, 4 ) == "cpt:" && IsSubStr( phase, ":failed" ) )
	{
		parts = StrTok( phase, ":" );
		if ( parts.size == 3 )
			was = parts[ 1 ];
	}
	if ( phase == "cpt:" + was + ":failed" )
	{
		foreach ( n, name in starts )
			if ( name == was )
				at = n;
		extra = "";
		if ( was != here )
			extra = ", the level's checkpoint for " + was;
		report( "cptour " + level.script + ": restarted at " + here + extra + " (" + ( at + 2 ) + " of " + ( starts.size + 1 ) + ")" );
		SetDvar( "mw2coop_qa_phase", "cpt:" + was + ":ok" );
		if ( flag_exist( "mw2coop_regrouped" ) )
			for ( t = 0; t < 10 && !flag( "mw2coop_regrouped" ); t += 0.5 )
				wait 0.5;
		wait 3;
		check( level.players.size == 2 && isdefined( level.player2 ), "two players after the restart (" + level.players.size + ")" );
		if ( was == here )
			check( GetDvar( "start" ) == here || ( here == "default" && ( GetDvar( "start" ) == "" || GetDvar( "start" ) == "default" ) ) || IsSubStr( GetDvar( "start" ), " ** " ), "the start point is still " + here + " ('" + GetDvar( "start" ) + "')" );
		tour_state( "after the restart" );
		qa_shot( "c-restart" );
	}
	else if ( phase == "cpt:" + here + ":ok" )
		report( "the mission failed again in section " + here + " (the level's own fail): skipped" );
	else
	{
		report( "cptour " + level.script + ": section " + here + " (" + ( at + 2 ) + " of " + ( starts.size + 1 ) + ")" );
		if ( flag_exist( "mw2coop_regrouped" ) )
			for ( t = 0; t < 10 && !flag( "mw2coop_regrouped" ); t += 0.5 )
				wait 0.5;
		if ( level.start_point == "default" )
			wait_intro();
		wait 3;
		check( level.players.size == 2 && isdefined( level.player2 ), "two players (" + level.players.size + ")" );
		SetDvar( "mw2coop_qa_phase", "cpt:" + here + ":failed" );
		tour_state( "before the failure" );
		if ( here != "default" )
		{
			// as a real checkpoint of this section: its name and where the allies stand
			SetDvar( "mw2coop_cp_start", here );
			maps\_mw2coop::snapshot_allies();
		}
		report( "failing the mission at " + here );
		level thread maps\_mw2coop::mission_failed();
		wait 40;
		report( "FAIL no restart 40 s after the failure at " + here );
		return;
	}
	if ( at + 1 < starts.size )
	{
		SetDvar( "start", starts[ at + 1 ] );
		report( "next section " + starts[ at + 1 ] );
		maps\_mw2coop::request( "restart" );
		return;
	}
	SetDvar( "mw2coop_qa_phase", "done" );
	report( "done" );
}

// the level's playable start points in order (debug and tweak starts left out, each name once)
tour_starts()
{
	names = [];
	seen = [];
	// The ending map in its knife fight mode ("credits_1", the way from af_chase, and what a tour launched straight onto the map gets): the
	// Museum's rooms are start points of its other modes (credits_2 starts at af_caves, the lobby's Museum is "free"). Here their dioramas
	// set up before the museum's animations exist, which this mode defines only when its credits reach the museum: about 40 script errors
	// a tour, none in play (night tour 2026-11-19).
	if ( level.script == "ending" && isdefined( level.level_mode ) && level.level_mode == "credits_1" )
		foreach ( room in strtok( "af_caves dc_burning airport cliffhanger favela hallway1 vehicles hallway2 oilrig estate hostage trainer arcadia", " " ) )
			seen[ room ] = true;
	foreach ( start in level.start_functions )
	{
		name = start[ "name" ];
		if ( !isdefined( seen[ name ] ) && !maps\_mw2coop::start_is_dev( name ) )
			names[ names.size ] = name;
		seen[ name ] = true;
	}
	return names;
}

tour_section()
{
	if ( flag_exist( "mw2coop_regrouped" ) )
		for ( t = 0; t < 10 && !flag( "mw2coop_regrouped" ); t += 0.5 )
			wait 0.5;
	if ( level.start_point == "default" )
		wait_intro();
	wait 2;
	check( level.players.size == 2 && isdefined( level.player2 ), "two players (" + level.players.size + ")" );
	tour_state( "spawn" );
	qa_shot( "a-spawn" );
	foreach ( player in level.players )
	{
		if ( player IsLinked() || !IsAlive( player ) )
			continue;
		check( !qa_stuck( player ), player_name( player ) + " is not stuck at the section's start (" + vec_int( player.origin ) + ")" );
		walked = qa_walk( player );	// before the text: GSC evaluates arguments last to first
		check( walked, player_name( player ) + " can walk (" + level.mw2coop_qa_walk + ")" );
	}
	wait 8;	// the section's own opening (allies move, enemies come)
	tour_state( "after 10 s" );
	qa_shot( "b-10s" );
	foreach ( n, trigger in tour_triggers( 2 ) )
	{
		label = "trigger " + ( n + 1 ) + " (" + trigger_text( trigger ) + ")";
		if ( level.player IsLinked() )
			break;
		level.player SetOrigin( trigger.mw2coop_floor );
		wait 4;	// player 2 follows a jump of player 1 (maps\_mw2coop::follow_leader_teleports)
		tour_state( label );
		qa_shot( "c-trigger" + ( n + 1 ) );
	}
}

// both players' state at a stop: up, apart, held by something
tour_state( where )
{
	text = where + ":";
	foreach ( player in level.players )
	{
		text += " " + player_name( player ) + " " + vec_int( player.origin );
		if ( !IsAlive( player ) )
			text += " DEAD";
		else if ( maps\_mw2coop::is_downed( player ) )
			text += " DOWN";
		if ( player IsLinked() )
			text += " linked";
	}
	apart = int( Distance( level.player.origin, level.player2.origin ) );
	// the Museum's black screens (level.black for everyone, the extra ones for the other players)
	if ( isdefined( level.black ) )
		text += " [black " + level.black.alpha + "]";
	if ( isdefined( level.mw2coop_museum_black ) )
		foreach ( overlay in level.mw2coop_museum_black )
			text += " [p2 black " + overlay.alpha + "]";
	report( text + "; " + apart + " apart" );
	check( IsAlive( level.player ) && IsAlive( level.player2 ) && !maps\_mw2coop::is_downed( level.player ) && !maps\_mw2coop::is_downed( level.player2 ), "both up " + where );
	// both riding something (separate vehicles, separate rigs) may be far apart; one riding and
	// the other left behind may not (dcburning heliride2: player 2 stayed at the level start)
	check( apart < 1500 || ( level.player IsLinked() && level.player2 IsLinked() ), "together " + where + " (" + apart + " units)" );
	// the allies: how far each is from the nearest player (owner's logs of 2026-11-04: squads 3500 to 9000 units behind after a restart at a checkpoint)
	count = 0;
	far = 0;
	worst = 0;
	worst_name = "-";
	foreach ( ai in GetAIArray( "allies" ) )
	{
		if ( !IsAlive( ai ) || isdefined( ai.vehicle ) )
			continue;
		// Wolverines: Raptor carries the president to the locker by a stock script, he is not a follower
		if ( IsSubStr( maps\_mw2coop::ally_name( ai ), "vip_invasion" ) )
			continue;
		count++;
		anchor = maps\_mw2coop::seek_player( ai.origin );
		d = int( Distance( ai.origin, anchor.origin ) );
		if ( d >= 2500 )
			far++;
		if ( d > worst )
		{
			worst = d;
			worst_name = maps\_mw2coop::ally_name( ai );
		}
	}
	report( where + ": allies " + count + ", farthest " + worst + " (" + worst_name + "), " + far + " at 2500 or more" );
	if ( where == "after the restart" )
		check( far == 0, "no ally 2500 or more units from the players " + where + " (" + far + " of " + count + ", farthest " + worst + " " + worst_name + ")" );
}

// the level's triggers 300 to 2500 units from player 1, nearest first: story triggers (they set a
// flag, script_flag) not yet passed, enemy spawn triggers and plain ones (dc_whitehouse has 27, 21
// and 32 of them, their origins at their centres)
tour_triggers( count )
{
	found = [];
	foreach ( class in strtok( "trigger_multiple_flag_set trigger_multiple_spawn trigger_multiple", " " ) )
		foreach ( trigger in GetEntArray( class, "classname" ) )
		{
			if ( trigger.origin == ( 0, 0, 0 ) )
				continue;
			if ( isdefined( trigger.script_flag ) && flag_exist( trigger.script_flag ) && flag( trigger.script_flag ) )
				continue;
			d = Distance( trigger.origin, level.player.origin );
			if ( d < 300 || d > 2500 )
				continue;
			// floor under its centre, free to stand on (a centre inside a wall or over a drop sent
			// player 1 6500 units down in the Gulag tour)
			floor = PlayerPhysicsTrace( trigger.origin + ( 0, 0, 48 ), trigger.origin - ( 0, 0, 256 ) );
			if ( floor[ 2 ] < trigger.origin[ 2 ] - 255 || qa_spot_stuck( floor ) )
				continue;
			trigger.mw2coop_floor = floor;
			found[ found.size ] = trigger;
		}
	picked = [];
	while ( picked.size < count && found.size )
	{
		best = 0;
		foreach ( n, trigger in found )
			if ( Distance( trigger.origin, level.player.origin ) < Distance( found[ best ].origin, level.player.origin ) )
				best = n;
		picked[ picked.size ] = found[ best ];
		found = array_remove_index( found, best );
	}
	return picked;
}

trigger_text( trigger )
{
	text = trigger.classname;
	if ( isdefined( trigger.script_flag ) )
		text += " " + trigger.script_flag;
	else if ( isdefined( trigger.targetname ) )
		text += " " + trigger.targetname;
	return text;
}

// A view from outside for the host's screen (player 1): his camera is moved to a point 80 units behind and
// 60 to the left of player 2, looking at him; whatever is linked to them in the scene stays there. The frame
// shows whether the two players' rigs and bodies overlap. Not for the client's screen.
qa_outside( label )
{
	p2 = level.player2;
	angles = ( 0, p2 GetPlayerAngles()[ 1 ], 0 );
	spot = p2.origin + AnglesToForward( angles ) * -80 + AnglesToRight( angles ) * -60 + ( 0, 0, 30 );
	cam = Spawn( "script_origin", spot );
	cam.angles = VectorToAngles( p2.origin + ( 0, 0, 20 ) - spot );
	level.player PlayerLinkTo( cam );
	wait 0.5;
	report( label + ": camera " + vec_int( spot ) + ", players " + int( Distance( level.player.origin, level.player2.origin ) ) + " apart" );
	qa_shot( label );
}

// both machines save their own frame (src/shot.c): <map>-<section>-<label>
// overlap: while the players are close, both look down, left, right and back (frames ovl-<n> of both views)
overlap_look()
{
	wait 2.5;
	if ( !isdefined( level.player2 ) || Distance( level.player GetEye(), level.player2 GetEye() ) > 60 )
		return;
	yaw1 = level.player GetPlayerAngles()[ 1 ];
	yaw2 = level.player2 GetPlayerAngles()[ 1 ];
	turns = [];
	turns[ 0 ] = ( 70, 0, 0 );
	turns[ 1 ] = ( 20, 90, 0 );
	turns[ 2 ] = ( 20, -90, 0 );
	turns[ 3 ] = ( 10, 180, 0 );
	foreach ( n, turn in turns )
	{
		level.player SetPlayerAngles( ( turn[ 0 ], yaw1 + turn[ 1 ], 0 ) );
		level.player2 SetPlayerAngles( ( turn[ 0 ], yaw2 + turn[ 1 ], 0 ) );
		wait 0.4;
		report( "overlap: look " + n + ": p1 angles " + vec_int( level.player GetPlayerAngles() ) + ", p2 " + vec_int( level.player2 GetPlayerAngles() ) );
		qa_shot( "ovl-" + n );
	}
}

// overlap: every 0.1 s the distance between the players' eyes and between their origins; logged when it changes by 10 units or more while under 150,
// a frame of both views (each PC writes its own) when the eyes are under 30 units, at most every 1.5 s, 12 frames
overlap_watch()
{
	level endon( "mw2coop_level_end" );
	last = -100;
	shots = 0;
	shot_at = -10000;
	min_eye = 100000;
	for ( ;; )
	{
		wait 0.1;
		if ( !isdefined( level.player2 ) )
			continue;
		d = Distance( level.player GetEye(), level.player2 GetEye() );
		d2 = Distance2D( level.player.origin, level.player2.origin );
		if ( d < min_eye )
			min_eye = d;
		if ( d < 150 && abs( d - last ) >= 10 )
		{
			report( "overlap: eyes " + int( d ) + ", origins 2D " + int( d2 ) + " dz " + int( level.player2.origin[ 2 ] - level.player.origin[ 2 ] ) + ", stances " + level.player GetStance() + "/" + level.player2 GetStance()
				+ ", linked " + level.player IsLinked() + "/" + level.player2 IsLinked() + ", t " + GetTime() + " (closest " + int( min_eye ) + ")" );
			last = d;
		}
		if ( d < 30 && shots < 12 && GetTime() - shot_at > 1500 )
		{
			shot_at = GetTime();
			shots++;
			qa_shot( "ov-" + shots );
		}
	}
}

qa_shot( label )
{
	SetDvar( "mw2coop_qa_shot", level.script + "-" + level.start_point + "-" + label );
	wait 0.6;
}

// The Barrett's scope on a pad (issue 28: "the sensitivity was 1000x, a very little movement of the pad moves the aim to the edge of the screen"):
// player 2 gets on the rifle, the right stick is pushed a little for 1 s; the turn is reported, unzoomed and zoomed in (the left stick forward, as
// the level teaches), with the scope's field of view (turretScopeZoom), so the turn can be read in screens.
barrettpad()
{
	wait_intro();
	wait 3;
	p2 = level.player2;
	pad = "mw2coop_qa_pad2";
	if ( GetDvar( "mw2coop_qa_who" ) == "1" || !isdefined( p2 ) )
	{
		p2 = level.player;	// (player 1 gets on the rifle instead: his QA pad, [native] p0pad, or his keyboard with mw2coop_qa_zoomkey)
		pad = "mw2coop_qa_pad1";
	}
	level.mw2coop_bp_pad = pad;
	check( isdefined( p2 ), "the player for the rifle is here" );
	trigger = GetEnt( "barrett_trigger", "targetname" );
	check( isdefined( trigger ), "the Barrett's use trigger is here" );
	p2 SetOrigin( trigger.origin );
	wait 0.5;
	trigger notify( "trigger", p2 );
	wait 3;
	check( flag( "player_is_on_turret" ), "the player is on the Barrett (" + maps\_mw2coop_dcburning::barrett_user() GetEntityNumber() + ")" );
	SetDvar( "mw2coop_qa_shot", level.script + "-bp-on" );
	wait 1;
	barrettpad_turn( p2, "unzoomed", 14000 );
	if ( GetDvar( "mw2coop_qa_zoomkey" ) == "1" )
		press( p2, "forward", 3000 );	// (the Forward key, as the keyboard presses it)
	else
		SetDvar( pad, "0 32767 0 0 0 0 0 1" );	// the left stick forward: the scope zooms in
	wait 3;
	SetDvar( pad, "0 0 0 0 0 0 0 1" );
	wait 1;
	SetDvar( "mw2coop_qa_shot", level.script + "-bp-zoomed" );
	wait 1;
	barrettpad_turn( p2, "zoomed", -14000 );	// (the other way: the rifle's arc ends not far to the right)
	// zoomed back out: the left stick back
	SetDvar( pad, "0 -32767 0 0 0 0 0 1" );
	wait 3;
	SetDvar( pad, "0 0 0 0 0 0 0 1" );
	wait 1;
	SetDvar( "mw2coop_qa_shot", level.script + "-bp-zoomedout" );
	wait 1;
	report( "done" );
}

barrettpad_turn( p, label, rx )
{
	pad = level.mw2coop_bp_pad;
	a0 = p GetPlayerAngles();
	SetDvar( pad, "0 0 " + rx + " 0 0 0 0 1" );
	wait 1;
	SetDvar( pad, "0 0 0 0 0 0 0 1" );
	wait 0.3;
	a1 = p GetPlayerAngles();
	turn = abs( AngleClamp180( a1[ 1 ] - a0[ 1 ] ) );
	report( "barrettpad " + label + ": the stick a little for 1 s turned the view " + turn + " degrees (cg_fov " + GetDvarFloat( "cg_fov" ) + ")" );
	SetDvar( "mw2coop_qa_shot", level.script + "-bp-" + label + "-after" );
	wait 1;
}


// Loose Ends' ending (estate.gsc ending_moments(), maps\_mw2coop_estate): both players go through it
// as Roach, at the same place, held there (Ghost's drag, the LZ, the body toss); Shepherd's shot
// hits both; nobody goes down; the mission ends. Logs both every 5 s.
betrayal()
{
	check( level.players.size == 2 && isdefined( level.player2 ), "two players (" + level.players.size + ")" );
	foreach ( player in level.players )
		player thread betrayal_hits();
	level thread betrayal_partner_fire();
	level thread betrayal_after_shot();
	phases = strtok( "play_ending_sequence begin_ending_music enter_the_littlebirds made_it_to_lz begin_overlapped_gasoline_sequence end_the_mission", " " );
	last = "";
	apart = 0;
	free = 0;
	down = 0;
	for ( t = 0; t < 150 && !flag( "end_the_mission" ); t++ )
	{
		phase = "";
		foreach ( f in phases )
			if ( flag_exist( f ) && flag( f ) )
				phase = f;
		if ( phase != last )
			report( "phase " + phase + " at " + t + " s" );
		last = phase;
		// from 2 s into the ending (the cut to black) both are held on Roach's view
		if ( t >= 4 )
		{
			apart = max( apart, Distance( level.player.origin, level.player2.origin ) );
			foreach ( player in level.players )
			{
				if ( !player IsLinked() )
					free++;
				if ( maps\_mw2coop::is_downed( player ) )
					down++;
			}
		}
		if ( t % 5 == 0 )
		{
			text = "t" + t;
			foreach ( player in level.players )
			{
				text += " | " + player_name( player ) + " " + vec_int( player.origin ) + " hp " + player.health;
				if ( player IsLinked() )
					text += " linked";
				text += " " + player GetCurrentWeapon();
			}
			maps\_mw2coop::state( text );
		}
		wait 1;
	}
	check( flag( "end_the_mission" ), "the ending ran to the mission's end (" + t + " s)" );
	check( apart < 2, "both players at Roach's place all through (at most " + int( apart ) + " units apart)" );
	check( free == 0, "both held on the view all through (" + free + " samples free)" );
	check( down == 0, "nobody went down (" + down + " samples)" );
	foreach ( player in level.players )
		check( isdefined( player.mw2coop_qa_low ) && player.mw2coop_qa_low <= 50, "Shepherd's shot hits " + player_name( player ) + " (health down to " + player.mw2coop_qa_low + ")" );
	SetDvar( "mw2coop_qa_phase", "done" );	// the next mission does not rerun it
	report( "done" );
}

// Owner 2026-11-17: held on one spot in Ghost's drag, player 1's fire at the Russians hurt player 2; and each player must be able
// to fire there himself. 6 s into the drag (both linked, both armed): each player in turn holds his trigger for 2.5 s (split
// screen: his pad dvar; network co-op: press(), on his own machine); his clip must go down and the other's health must not.
// Then a partner hit is made the game's way (DoDamage with the other player as the attacker). The lobby's friendly fire is
// on in this run (MW2COOP_PRE / MW2COOP_HOST_CMDS "set mw2coop_ff 1").
betrayal_partner_fire()
{
	flag_wait( "play_ending_sequence" );
	// the drag's rifle comes some 12 s in (before that both hold "none": a first version pressed then and proved nothing)
	for ( t = 0; t < 40 && !flag( "made_it_to_lz" ); t++ )
	{
		armed = 0;
		foreach ( player in level.players )
			if ( player GetCurrentWeapon() != "none" )
				armed++;
		if ( armed == level.players.size )
			break;
		wait 0.5;
	}
	wait 1.5;
	if ( flag( "made_it_to_lz" ) )
		return;
	split = GetDvarInt( "mw2coop_native_split" ) != 0;
	foreach ( n, shooter in level.players )
	{
		other = level.player2;
		if ( shooter == level.player2 )
			other = level.player;
		weapon = shooter GetCurrentWeapon();
		clip = shooter GetWeaponAmmoClip( weapon );
		before = other.health;
		low = before;
		if ( split )
			SetDvar( "mw2coop_qa_pad" + ( n + 1 ), "0 0 0 0 0 0 255 0" );
		else
			press( shooter, "attack", 2500 );
		for ( i = 0; i < 25; i++ )
		{
			low = min( low, other.health );
			wait 0.1;
		}
		if ( split )
			SetDvar( "mw2coop_qa_pad" + ( n + 1 ), "0 0 0 0 0 0 0 0" );
		wait 0.5;
		after = shooter GetWeaponAmmoClip( weapon );
		report( "partner fire: " + player_name( shooter ) + " held his trigger (" + weapon + ", clip " + clip + " -> " + after + "); " + player_name( other ) + "'s health " + before + " -> " + other.health + " (lowest " + low + "), ff dvar " + GetDvar( "mw2coop_ff" ) + ", scale " + GetDvar( "g_friendlyfireDamageScale" ) );
		check( after < clip, player_name( shooter ) + " fires himself in the drag (his clip went down)" );
		check( other.health >= before, player_name( shooter ) + "'s fire does not cost " + player_name( other ) + " health while they are held together" );
	}
	p1 = level.player.health;
	p2 = level.player2.health;
	level.player2 DoDamage( 40, level.player.origin, level.player );
	level.player DoDamage( 40, level.player2.origin, level.player2 );
	wait 0.3;
	report( "partner hits: player 1 " + p1 + " -> " + level.player.health + ", player 2 " + p2 + " -> " + level.player2.health );
	check( level.player2.health >= p2 && level.player.health >= p1, "a hit from the partner costs nothing in the ending (both back to their health)" );
}

// Owner 2026-11-17: player 2's screen went black at Shepherd's shot (maps\_mw2coop_estate::partner_shot). 1.5 s after the
// LZ's shot: no black screen element for him, and both frames are saved (mw2coop\shots) to look at.
betrayal_after_shot()
{
	flag_wait( "made_it_to_lz" );
	while ( !flag( "end_the_mission" ) && level.player.health >= level.player.maxhealth )
		wait 0.05;
	wait 1.5;
	check( !isdefined( level.mw2coop_estate_black ), "no black screen for player 2 after Shepherd's shot" );
	qa_shot( "aftershot" );
}

// his lowest health after the LZ (Shepherd's pistol; the ending's death shield sends no damage
// notify, and his health is full again 0.3 s later)
betrayal_hits()
{
	self endon( "death" );
	flag_wait( "made_it_to_lz" );
	self.mw2coop_qa_low = self.health;
	while ( !flag( "end_the_mission" ) )
	{
		self.mw2coop_qa_low = min( self.mw2coop_qa_low, self.health );
		wait 0.05;
	}
}

// Range starts (trainer.gsc firing_range_init() -> maps\_mw2coop_trainer::range_places): both
// players inside the sandbag bay, not stuck.
bay()
{
	check( level.players.size == 2 && isdefined( level.player2 ), "two players (" + level.players.size + ")" );
	level.mw2coop_no_follow = true;
	wait_intro();
	start = GetEnt( "player_start_range", "targetname" );
	foreach ( player in level.players )
	{
		check( qa_in_bay( player, start ), player_name( player ) + " is inside the sandbags (" + bay_text( player, start ) + ")" );
		check( !qa_stuck( player ), player_name( player ) + " is not stuck in anything solid" );
	}
	report( "done" );
}

// S.S.D.D.: after the two targets from the hip, single player asks player 1 whether to invert the look (a popup, his controls frozen, the screen
// blurred, until he answers); co-op leaves the question out (trainer.gsc.patch 9). Both get the range's rifle, player 1 faces the targets, and each
// target that pops up is hit as player 1's shot would hit it; the level must go on to the sights training (flag hip_fire_done) with nobody frozen.
invertq()
{
	check( level.players.size == 2 && isdefined( level.player2 ), "two players (" + level.players.size + ")" );
	level.mw2coop_no_follow = true;
	wait_intro();
	foreach ( player in level.players )
		if ( !player HasWeapon( level.gunPrimary ) )
		{
			player GiveWeapon( level.gunPrimary );
			player SwitchToWeapon( level.gunPrimary );
		}
	t0 = GetTime();
	while ( !flag( "player_facing_targets_for_hip_fire" ) && GetTime() - t0 < 90000 )
	{
		if ( isdefined( level.firing_range_area ) )
			level.player SetPlayerAngles( VectorToAngles( level.firing_range_area.origin - level.player GetEye() ) );
		wait 0.5;
	}
	check( flag( "player_facing_targets_for_hip_fire" ), "the hip fire part began (player 1 faces the targets, " + ( GetTime() - t0 ) + " ms)" );
	hits = 0;
	t1 = GetTime();
	while ( !flag( "hip_fire_done" ) && GetTime() - t1 < 40000 )
	{
		foreach ( target in level.hip_targets )
			target notify( "damage", 100, level.player, ( 0, 0, 1 ), target.origin, "MOD_RIFLE_BULLET" ); // (a target that is down does not listen)
		hits++;
		wait 1;
	}
	SetDvar( "mw2coop_qa_shot", level.script + "-invertq" );
	// (the question waits for player 1's answer with his controls frozen: while it is up the level does not get there)
	check( flag( "hip_fire_done" ), "the level went on to the sights training after the two targets, no question whether to invert the look (" + ( GetTime() - t1 ) + " ms, " + hits + " rounds of hits)" );
	wait 2;
	SetDvar( "mw2coop_qa_shot", level.script + "-invertq-after" );
	report( "done" );
}

// The partner's distance on the game's marker of him (src/markers.c draws it with the icon and answers markers_expect() in the log, "qa: markers <step>:
// PASS|FAIL"): player 2 stands 500 units (13 m) in front of player 1, who looks at him (the game shows no marker of a partner in view, so no distance
// either), away (the marker at the screen's edge with "13 m"), round (behind him), then player 2 is downed (in view: no marker, the game's "Partner down"
// line is in the middle) and player 1 looks away again (the health icon at the edge with "13 m"). Run with MW2COOP_GOD=0 (a down needs damage).
// partnernear (owner 2026-11-26: "closer than 5 metres, no distance and the marker disappears"): player 2 stands behind player 1 (his marker pinned to the
// view's edge, with the distance) at 8, 3, 5.1 (back from near: still hidden up to 5.3), 6 and 4.5 m; player 1's view must draw it at 8 and 6 m only
partnernear()
{
	check( level.players.size == 2 && isdefined( level.player2 ), "two players (" + level.players.size + ")" );
	level.mw2coop_no_follow = true;
	wait_intro();
	wait 2;
	foreach ( player in level.players )
		player EnableInvulnerability();
	yaw = level.player GetPlayerAngles()[ 1 ];
	fwd = AnglesToForward( ( 0, yaw, 0 ) );
	spot = level.player.origin;
	steps = [];
	steps[ 0 ] = 8;
	steps[ 1 ] = 3;
	steps[ 2 ] = 5.1;
	steps[ 3 ] = 6;
	steps[ 4 ] = 4.5;
	foreach ( n, metres in steps )
	{
		level.player SetOrigin( spot );
		level.player SetPlayerAngles( ( 0, yaw, 0 ) );
		level.player2 SetOrigin( spot - fwd * ( metres / 0.0254 ) );
		level.player2 SetPlayerAngles( ( 0, yaw, 0 ) );
		wait 1;
		report( "partnernear: player 2 " + metres + " m behind (eyes " + int( Distance( level.player GetEye(), level.player2 GetEye() ) ) + " units)" );
		if ( metres == 8 || metres == 6 )
			markers_expect( "at-" + metres, "coop_player_location", int( metres ) - 1, int( metres ) + 1 );
		else
			markers_expect( "at-" + metres, "coop_player_location", -1, -1 );
		SetDvar( "mw2coop_qa_shot", level.script + "-pn-" + n );
		wait 1.5;
	}
	report( "done" );
}

partnerline()
{
	check( level.players.size == 2 && isdefined( level.player2 ), "two players (" + level.players.size + ")" );
	level.mw2coop_no_follow = true;
	wait_intro();
	wait 2;
	foreach ( player in level.players )
		player EnableInvulnerability();
	yaw = level.player GetPlayerAngles()[ 1 ];
	fwd = AnglesToForward( ( 0, yaw, 0 ) );
	level.player2 SetOrigin( level.player.origin + fwd * 500 );
	level.player2 SetPlayerAngles( ( 0, yaw + 180, 0 ) );
	level.player SetPlayerAngles( ( 0, yaw, 0 ) );
	wait 1;
	markers_expect( "seen", "coop_player_location", -1, -1 );
	SetDvar( "mw2coop_qa_shot", level.script + "-pl-seen" );
	wait 1.5;
	level.player SetPlayerAngles( ( 0, yaw + 100, 0 ) );	// (turned left: the partner is on his right)
	wait 1;
	markers_expect( "away", "coop_player_location", 11, 15 );
	SetDvar( "mw2coop_qa_shot", level.script + "-pl-away" );
	wait 1.5;
	level.player SetPlayerAngles( ( 0, yaw + 180, 0 ) );
	wait 1;
	markers_expect( "behind", "coop_player_location", 11, 15 );
	SetDvar( "mw2coop_qa_shot", level.script + "-pl-behind" );
	wait 1.5;
	level.player SetPlayerAngles( ( 0, yaw, 0 ) );
	wait 0.5;
	level.player2 DisableInvulnerability();
	level.player2 DoDamage( level.player2.health + 500, level.player2.origin );
	wait 1.5;
	check( level.player2 ent_flag_exist( "coop_downed" ) && level.player2 ent_flag( "coop_downed" ), "player 2 is down" );
	markers_expect( "down-seen", "hint_health", -1, -1 );
	SetDvar( "mw2coop_qa_shot", level.script + "-pl-down" );
	wait 1.5;
	level.player SetPlayerAngles( ( 0, yaw + 120, 0 ) );
	wait 1;
	markers_expect( "down-away", "hint_health", 11, 15 );
	SetDvar( "mw2coop_qa_shot", level.script + "-pl-downaway" );
	wait 1.5;
	report( "done" );
}

// Asks the mod what the markers' draws show on player 1's screen: src/markers.c answers half a second later in the log, "qa: markers <tag>: PASS|FAIL"
// (least -1: the icon must not be drawn). The answer is not read back here: a value that differs between the PCs of a network session must not reach the
// script (the game adds what GetDvar returns to the lockstep state hash, and netwatch then reloads a checkpoint).
markers_expect( tag, icon, least, most )
{
	SetDvar( "mw2coop_qa_markers_expect", tag + " " + icon + " " + least + " " + most );
	wait 0.8;
}

// The wheel's "on me" and "need ammo" (maps\_mw2coop_ping, maps\_mw2coop_partner): player 2 is put some way off, player 1 picks "on me" on the wheel (sector 9)
// and player 2 must be brought beside him; then player 2 picks "need ammo" (sector 8), player 1 looks at him and presses Use (real input, press()): player 2's
// weapon gets a magazine from player 1's reserve. The voices are checked where the level loads its side's lines (Invasion: Rangers).
wheelnew()
{
	check( level.players.size == 2 && isdefined( level.player2 ), "two players (" + level.players.size + ")" );
	level.mw2coop_no_follow = true;
	wait_intro();
	wait 2;
	p1 = level.player;
	p2 = level.player2;
	foreach ( player in level.players )
		player EnableInvulnerability();
	// both armed (S.S.D.D. hands out the range's rifle at the table only: no weapon, no ammo to give)
	foreach ( player in level.players )
		if ( isdefined( level.gunPrimary ) && !player HasWeapon( level.gunPrimary ) )
		{
			player GiveWeapon( level.gunPrimary );
			player GiveMaxAmmo( level.gunPrimary );
			player SwitchToWeapon( level.gunPrimary );
		}
	cw = p2 GetCurrentWeapon();
	if ( isdefined( level.gunPrimary ) && p2 HasWeapon( level.gunPrimary ) )
		cw = level.gunPrimary; // (S.S.D.D.: the rifle just given, still being switched to)
	if ( isdefined( cw ) && cw != "none" )
		p2 SetWeaponAmmoStock( cw, 0 ); // (room for the magazine he is given, whatever the level hands out)
	wait 1.5;
	// player 2 500 units in front of player 1 (as partnerline: the start's tent has no room for a trace to find)
	yaw = p1 GetPlayerAngles()[ 1 ];
	p2 SetOrigin( p1.origin + AnglesToForward( ( 0, yaw, 0 ) ) * 500 );
	moved = true;
	wait 0.5;
	far = Distance( p1.origin, p2.origin );
	check( moved && far > 320, "player 2 is put some way off (" + int( far ) + " units)" );
	// "on me"
	p1 maps\_mw2coop_ping::event( "open", -1 );
	wait 0.3;
	check( isdefined( p1.mw2coop_wheel ) && p1.mw2coop_wheel.boxes.size == 10, "the wheel has ten items" );
	p1 maps\_mw2coop_ping::event( "sel", 9 );
	wait 0.4;
	qa_shot( "wheel-come" );
	p1 maps\_mw2coop_ping::event( "close", 9 );
	wait 0.6;
	last = ping_last( p1 );
	check( isdefined( last ) && last.kind == "come", "'on me' made its marker (" + ping_kinds( p1 ) + ")" );
	c = maps\_mw2coop_ping::voice_country();
	if ( c != "" )
		check( isdefined( last ) && IsSubStr( ping_voice( last ), "_1_order_move" ), "'on me' in player 1's voice (" + ping_voice( last ) + ")" );
	wait 5;
	near = Distance( p1.origin, p2.origin );
	check( near < 160, "player 2 was brought beside player 1 (" + int( far ) + " -> " + int( near ) + " units)" );
	qa_shot( "wheel-came" );
	// "need ammo" from player 2, player 1 gives
	wait 2.5;	// (the presses' guard)
	w2 = p2 GetCurrentWeapon();
	w1 = p1 GetCurrentWeapon();
	before2 = p2 GetWeaponAmmoStock( w2 );
	before1 = p1 GetWeaponAmmoStock( w1 );
	p2 maps\_mw2coop_ping::event( "open", -1 );
	wait 0.3;
	p2 maps\_mw2coop_ping::event( "sel", 8 );
	wait 0.3;
	p2 maps\_mw2coop_ping::event( "close", 8 );
	wait 0.8;
	last = ping_last( p2 );
	check( isdefined( last ) && last.kind == "needammo" && isdefined( p2.mw2coop_ammo_asked ), "'need ammo' made its marker on player 2 (" + ping_kinds( p2 ) + ")" );
	if ( c != "" )
		report( "wheelnew: 'need ammo' voice: " + ping_voice( last ) );
	// player 1 stands 50 units in front of player 2, faces his chest and presses Use (the game's +activate, then +usereload, the revive's key)
	yaw2 = p2 GetPlayerAngles()[ 1 ];
	p1 SetOrigin( p2.origin + AnglesToForward( ( 0, yaw2, 0 ) ) * 50 );
	wait 0.3;
	p1 SetPlayerAngles( VectorToAngles( ( p2.origin + ( 0, 0, 40 ) ) - p1 GetEye() ) );
	wait 1;
	qa_shot( "wheel-ammohint" );
	markers_expect( "ammo", "waypoint_ammo", 0, 3 );	// (the marker over player 2's head, 50 units away, with its distance: src/markers.c)
	held = 0;
	foreach ( button in StrTok( "activate usereload", " " ) )
	{
		press( p1, button, 750 );
		for ( t = 0; t < 10; t++ )
		{
			if ( p1 UseButtonPressed() )
				held++;
			wait 0.1;
		}
		wait 0.8;
		if ( p2 GetWeaponAmmoStock( w2 ) > before2 )
			break;
	}
	after2 = p2 GetWeaponAmmoStock( w2 );
	after1 = p1 GetWeaponAmmoStock( w1 );
	report( "wheelnew: player 1's Use seen held by the server in " + held + " of 20 looks, " + int( Distance( p1.origin, p2.origin ) ) + " units from player 2" );
	check( after2 > before2 && after1 < before1, "player 1's Use gave player 2 ammo (player 2 " + w2 + " " + before2 + " -> " + after2 + ", player 1 " + w1 + " " + before1 + " -> " + after1 + ")" );
	qa_shot( "wheel-ammogiven" );
	if ( after2 <= before2 && isdefined( p2.mw2coop_ammo_use ) )
	{ // (the give itself, without the use detection: the use entity's trigger as the engine sends it)
		p2.mw2coop_ammo_use notify( "trigger", p1 );
		wait 0.5;
		check( p2 GetWeaponAmmoStock( w2 ) > before2, "the give itself works when the use entity is triggered (player 2 " + before2 + " -> " + p2 GetWeaponAmmoStock( w2 ) + ")" );
	}
	// a full reserve: asked or not, no offer, and a Use takes nothing from the giver (it cost him a magazine for nothing, QA 2026-11-24)
	if ( w2 != "none" && w1 != "none" )
	{
		p2 SetWeaponAmmoStock( w2, WeaponMaxAmmo( w2 ) );
		p2.mw2coop_ammo_asked = GetTime();
		wait 0.8;
		b1 = p1 GetWeaponAmmoStock( w1 );
		if ( isdefined( p2.mw2coop_ammo_use ) )
			p2.mw2coop_ammo_use notify( "trigger", p1 );
		wait 0.5;
		check( !( p2 maps\_mw2coop_partner::ammo_needed() ), "a full partner is offered no ammo" );
		check( p1 GetWeaponAmmoStock( w1 ) == b1, "a full partner takes nothing from the giver (player 1 " + b1 + " -> " + p1 GetWeaponAmmoStock( w1 ) + ")" );
	}
	report( "done" );
}

// The obstacle course: the entrance gate closes once both players are upstairs, not when the
// first one gets there (the other was locked out or in).
// The end of the timed run: the exit gate closes (door_close waiting for player_course_end_close_gate) only once both players are through that trigger.
course2()
{
	check( level.players.size == 2 && isdefined( level.player2 ), "two players (" + level.players.size + ")" );
	level.mw2coop_no_follow = true;
	wait_intro();
	trigs = maps\_mw2coop_trainer::course_triggers( "player_course_end_close_gate" );
	check( trigs.size > 0, "a trigger sets player_course_end_close_gate (" + trigs.size + ")" );
	ends = maps\_mw2coop_trainer::course_triggers( "player_course_end" );
	if ( !trigs.size || !ends.size )
	{
		report( "done" );
		return;
	}
	// the course loop is not run: the course's own door_close waits for the flag, as the loop calls it at the end
	door_close = level.mw2coop_door_close;
	check( isdefined( door_close ), "the course's door_close is known" );
	level.gate_cqb_exit thread [[ door_close ]]( "player_course_end_close_gate" );
	gate = level.gate_cqb_exit;
	yaw = gate.angles[ 1 ];
	level.player SetOrigin( trigs[ 0 ].origin );
	wait 3;
	report( "player 1 at the closing trigger: flag " + flag( "player_course_end_close_gate" ) + ", exit gate turned " + int( gate.angles[ 1 ] - yaw ) );
	check( Abs( gate.angles[ 1 ] - yaw ) < 1, "the exit gate stays open while player 2 is behind" );
	level.player2 SetOrigin( trigs[ 0 ].origin );
	wait 4;
	report( "player 2 at the closing trigger: gate turned " + int( gate.angles[ 1 ] - yaw ) );
	check( Abs( gate.angles[ 1 ] - yaw ) > 45, "the exit gate closes once both are through" );
	report( "done" );
}

course()
{
	check( level.players.size == 2 && isdefined( level.player2 ), "two players (" + level.players.size + ")" );
	level.mw2coop_no_follow = true;
	wait_intro();
	names = [];
	names[ 0 ] = "player_course_upstairs";
	names[ 1 ] = "player_course_end";
	foreach ( name in names )
		check( maps\_mw2coop_trainer::course_triggers( name ).size > 0, "a trigger sets " + name + " (" + maps\_mw2coop_trainer::course_triggers( name ).size + ")" );
	up = maps\_mw2coop_trainer::course_triggers( "player_course_upstairs" )[ 0 ];
	if ( !isdefined( up ) )
	{
		report( "done" );
		return;
	}
	// the course loop: start, then the stretch before the stairs
	flags = [];
	flags[ 0 ] = "player_has_started_course";
	flags[ 1 ] = "player_course_03a";
	flags[ 2 ] = "player_course_stairs2";
	foreach ( name in flags )
	{
		flag_set( name );
		wait 0.5;
	}
	gate = level.gate_cqb_enter;
	yaw = gate.angles[ 1 ];
	level.player SetOrigin( up.origin );
	wait 3;
	report( "player 1 upstairs: flag " + flag( "player_course_upstairs" ) + ", gate turned " + int( gate.angles[ 1 ] - yaw ) );
	check( flag( "player_course_upstairs" ), "player 1 set the upstairs flag" );
	check( Abs( gate.angles[ 1 ] - yaw ) < 1, "the entrance gate stays open while player 2 is behind" );
	level.player2 SetOrigin( up.origin + ( 30, 0, 0 ) );
	wait 3;
	report( "player 2 upstairs: gate turned " + int( gate.angles[ 1 ] - yaw ) );
	check( Abs( gate.angles[ 1 ] - yaw ) > 45, "the entrance gate closes once both are upstairs" );
	report( "done" );
}

// Inside the range's sandbag bay: between its ends (101 units left to 28 right of
// player_start_range), the bags close in front and the back wall close behind. Measured with
// player traces, 2026-10-22: from player 1's place the bags are 37 units ahead and the wall 7
// behind; outside, one of the two is open for 120 units.
qa_in_bay( player, start )
{
	right = AnglesToRight( ( 0, start.angles[ 1 ], 0 ) );
	fwd = AnglesToForward( ( 0, start.angles[ 1 ], 0 ) );
	side = VectorDot( player.origin - start.origin, right );
	at = player.origin + ( 0, 0, 2 );
	front = Distance( at, PlayerPhysicsTrace( at, at + fwd * 120 ) );
	back = Distance( at, PlayerPhysicsTrace( at, at - fwd * 120 ) );
	return side > -101 && side < 28 && front < 60 && back < 40;
}

bay_text( player, start )
{
	right = AnglesToRight( ( 0, start.angles[ 1 ], 0 ) );
	fwd = AnglesToForward( ( 0, start.angles[ 1 ], 0 ) );
	at = player.origin + ( 0, 0, 2 );
	return int( VectorDot( player.origin - start.origin, right ) ) + " right of the start, bags " + int( Distance( at, PlayerPhysicsTrace( at, at + fwd * 120 ) ) ) + " ahead, wall " + int( Distance( at, PlayerPhysicsTrace( at, at - fwd * 120 ) ) ) + " behind";
}

// The box (trainer.gsc spawn_frags()): its trigger is a touch trigger and the level checks that
// he looks at the box, so each player walks up to it (real Forward presses) and looks at it.
pickup_frags()
{
	flag_wait( "frags_have_been_spawned" );
	box = GetEnt( "grenade_box", "targetname" );
	trigger = GetEnt( "frag_trigger", "script_noteworthy" );
	foreach ( n, player in level.players )
	{
		from = int( Distance2D( player.origin, box.origin ) );
		for ( i = 0; i < 16 && !player IsTouching( trigger ); i++ )
		{
			player SetPlayerAngles( ( 0, VectorToAngles( box.origin - player.origin )[ 1 ], 0 ) );
			press( player, "forward", 250 );
			wait 0.2;
		}
		report( player_name( player ) + " walked up to the box: " + i + " Forward presses, from " + from + " to " + int( Distance2D( player.origin, box.origin ) ) + " units from it" );
		for ( i = 0; i < 10 && player GetWeaponAmmoStock( "fraggrenade" ) < 3; i++ )
		{
			player SetPlayerAngles( VectorToAngles( box.origin - player GetEye() ) );
			wait 0.5;
		}
		check( player HasWeapon( "fraggrenade" ) && player GetWeaponAmmoStock( "fraggrenade" ) >= 3, "player " + ( n + 1 ) + " takes frags from the box (" + player GetWeaponAmmoStock( "fraggrenade" ) + ")" );
		// and steps back out of the other player's way
		player SetPlayerAngles( ( 0, VectorToAngles( box.origin - player.origin )[ 1 ], 0 ) );
		for ( i = 0; i < 4; i++ )
			press( player, "back", 250 );
	}
	report( "done" );
}

qa_stand_by( player, at, dist )
{
	if ( !isdefined( dist ) )
		dist = 40;
	away = ( player.origin[ 0 ] - at[ 0 ], player.origin[ 1 ] - at[ 1 ], 0 );
	if ( Length( away ) < 1 )
		away = ( 1, 0, 0 );
	// on the floor below that spot (a weapon lies on a table, the player may start on another level)
	spot = at + VectorNormalize( away ) * dist;
	player SetOrigin( PlayerPhysicsTrace( spot + ( 0, 0, 16 ), spot - ( 0, 0, 200 ) ) );
	wait 0.1;
}

// A table pickup by hand: he stands by the weapon, looks at it and presses Use (up to 4 tries);
// then he walks back to where he stood.
qa_use_take( player, item, weapon )
{
	back = player.origin;
	qa_stand_at_table( player, item.origin );
	for ( i = 0; i < 4 && !player HasWeapon( weapon ); i++ )
	{
		if ( isdefined( item ) )
			player SetPlayerAngles( VectorToAngles( item.origin - player GetEye() ) );
		wait 0.2;
		press( player, "activate", 500 );
		for ( t = 0; t < 2 && !player HasWeapon( weapon ); t += 0.25 )
			wait 0.25;
		if ( !player HasWeapon( weapon ) && isdefined( item ) )
			report( player_name( player ) + ": no " + weapon + " after Use " + ( i + 1 ) + " (" + int( Distance( player GetEye(), item.origin ) ) + " units from the eye to it; holds " + weapons_text( player ) + ")" );
	}
	report( player_name( player ) + " pressed Use " + i + " times for the " + weapon );
	player SetOrigin( back );
	wait 0.5;
}

// Where a player would stand to take something off a table: 40 units from it on the floor (not on
// the sandbags beside it), free all round, clear of the other player, with a clear view of it.
// Tried from his own side first.
qa_stand_at_table( player, at )
{
	other = level.player;
	if ( player == level.player )
		other = level.player2;
	yaw = VectorToAngles( ( player.origin[ 0 ] - at[ 0 ], player.origin[ 1 ] - at[ 1 ], 0 ) )[ 1 ];
	turns = strtok( "0 45 -45 90 -90 135 -135 180", " " );
	grounds = [];
	floor_z = undefined;
	foreach ( turn in turns )
	{
		spot = at + AnglesToForward( ( 0, yaw + int( turn ), 0 ) ) * 40;
		grounds[ turn ] = PlayerPhysicsTrace( spot + ( 0, 0, 16 ), spot - ( 0, 0, 200 ) );
		if ( !isdefined( floor_z ) || grounds[ turn ][ 2 ] < floor_z )
			floor_z = grounds[ turn ][ 2 ];
	}
	foreach ( turn in turns )
	{
		ground = grounds[ turn ];
		if ( ground[ 2 ] > floor_z + 8 || qa_spot_stuck( ground ) || Distance2D( ground, other.origin ) < 48 )
			continue;
		if ( !BulletTracePassed( ground + ( 0, 0, 60 ), at, false, undefined ) )
			continue;
		player SetOrigin( ground );
		wait 0.1;
		return;
	}
	report( player_name( player ) + ": no free floor by the table, standing by it anyway" );
	qa_stand_by( player, at );
}

// The level's intro (maps\_introscreen) holds the players until its title has gone. Most intros
// set the flag introscreen_complete at the end, some only notify it (dc_whitehouse); none takes
// 15 s.
// The trainer's frag box (owner, 2026-10-29: grenades could not be picked up in the split screen): each player in turn stands at the box, looks at it and presses Use; his frag count
// must go up (start the level at the frags with "set start frag")
frags()
{
	check( level.players.size == 2 && isdefined( level.player2 ), "two players (" + level.players.size + ")" );
	flag_wait( "frags_have_been_spawned" );
	wait 4;
	box = GetEnt( "grenade_box", "targetname" );
	check( isdefined( box ), "the grenade box exists" );
	if ( !isdefined( box ) )
		return;
	trig = GetEnt( "frag_trigger", "script_noteworthy" );
	report( "grenade box at " + box.origin + ", frag trigger " + trig.classname + " at " + trig.origin );
	foreach ( n, player in level.players )
	{
		before = player GetWeaponAmmoStock( "fraggrenade" );
		org = ( trig.origin[ 0 ], trig.origin[ 1 ] + ( n * 8 ), player.origin[ 2 ] );
		player SetOrigin( org );
		wait 0.5;
		player SetPlayerAngles( VectorToAngles( box.origin - player GetEye() ) );
		wait 1;
		report( "frags: " + player_name( player ) + " at " + int( Distance2D( player.origin, box.origin ) ) + " from the box, frags " + before + ", presses Use" );
		press( player, "activate", 1500 );
		wait 2.5;
		after = player GetWeaponAmmoStock( "fraggrenade" );
		check( after > before, "frags: " + player_name( player ) + " picked up grenades (" + before + " -> " + after + ")" );
		SetDvar( "mw2coop_qa_frame", "frags-" + player_name( player ) );
		wait 1.5;
	}
	// then the players face each other and fire in front of the cameras: the muzzle flash must be only at the gun that fires
	foreach ( player in level.players )
	{
		player GiveWeapon( "m4_grunt" );
		player GiveMaxAmmo( "m4_grunt" );
		player SwitchToWeapon( "m4_grunt" );
	}
	fwd = AnglesToForward( ( 0, level.player GetPlayerAngles()[ 1 ], 0 ) );
	level.player2 SetOrigin( level.player.origin + fwd * 90 );
	wait 1;
	level.player SetPlayerAngles( VectorToAngles( ( level.player2.origin + ( 0, 0, 48 ) ) - level.player GetEye() ) );
	level.player2 SetPlayerAngles( VectorToAngles( ( level.player.origin + ( 0, 0, 48 ) ) - level.player2 GetEye() ) );
	wait 2;
	foreach ( player in level.players )
	{
		report( "frags: " + player_name( player ) + " fires" );
		press( player, "attack", 5000 );
		wait 1.5;
		SetDvar( "mw2coop_qa_frame", "fire-" + player_name( player ) + "-a" );
		wait 2;
		SetDvar( "mw2coop_qa_frame", "fire-" + player_name( player ) + "-b" );
		wait 3;
		SetDvar( "mw2coop_qa_frame", "fire-" + player_name( player ) + "-after" );
		wait 1;
	}
	report( "done" );
}

// Native split screen: one player fires at the other, who looks at him; the composed frames (both halves) every 0.4 s: the shooter's muzzle flash must show in the other half too
// A single effect at a fixed spot in front of both players, so its age in each half can be compared (the effects clock advanced twice a frame in mode 11:
// smoke and flashes lived one step ahead of the view that drew them). Two captures a second apart from the split.
fxhalf()
{
	wait_intro();
	wait 5;
	spot = level.player.origin + ( AnglesToForward( level.player.angles ) * 250 );
	spot = ( spot[0], spot[1], spot[2] + 40 );
	maps\_mw2coop::state( "fxhalf: effect at " + vec_int( spot ) );
	// The effect table is the level's and the trainer's zone has no explosion effect registered, so use the engine's own: a scripted explosion at a fixed spot
	// (the same call the game uses for a grenade) makes the effect the game itself spawns for it, in both halves.
	report( "fxhalf: explosion at " + vec_int( spot ) );
	RadiusDamage( spot, 200, 200, 50, level.player );

	wait 0.35;
	SetDvar( "mw2coop_qa_frame", "fxhalf-a" );
	wait 1;
	SetDvar( "mw2coop_qa_frame", "fxhalf-b" );
	wait 1;
	report( "fxhalf done at " + vec_int( spot ) );
}

firecheck()
{
	check( level.players.size == 2 && isdefined( level.player2 ), "two players (" + level.players.size + ")" );
	wait_intro();
	wait 6;
	foreach ( player in level.players )
	{
		player EnableWeapons();
		player FreezeControls( false );
		player GiveWeapon( "m4_grunt" );
		player GiveMaxAmmo( "m4_grunt" );
		player SwitchToWeapon( "m4_grunt" );
	}
	fwd = AnglesToForward( ( 0, level.player GetPlayerAngles()[ 1 ], 0 ) );
	level.player2 SetOrigin( level.player.origin + fwd * 160 );
	wait 1;
	level.player SetPlayerAngles( VectorToAngles( ( level.player2.origin + ( 0, 0, 48 ) ) - level.player GetEye() ) );
	level.player2 SetPlayerAngles( VectorToAngles( ( level.player.origin + ( 0, 0, 48 ) ) - level.player2 GetEye() ) );
	wait 2;
	foreach ( player in level.players )
	{
		report( "firecheck: " + player_name( player ) + " fires, " + player GetCurrentWeapon() );
		press( player, "attack", 4000 );
		for ( i = 0; i < 8; i++ )
		{
			wait 0.4;
			SetDvar( "mw2coop_qa_frame", "fc-" + player_name( player ) + "-" + i );
		}
		wait 2;
	}
	report( "done" );
}

// Native split screen: player 2 keeps his view, player 1 turns around in steps; the composed frames: do the shadows in player 2's half depend on where player 1 looks?
shadowcheck()
{
	check( level.players.size == 2 && isdefined( level.player2 ), "two players (" + level.players.size + ")" );
	flag_wait( "frags_have_been_spawned" );
	wait 4;
	box = GetEnt( "grenade_box", "targetname" );
	// player 2 looks straight at the table with the grenade box (the camouflage net over it is in view); player 1 beside him turns in 90 degree steps and stands still for each frame
	level.player2 FreezeControls( true );
	to = VectorToAngles( ( box.origin[ 0 ], box.origin[ 1 ], level.player2 GetEye()[ 2 ] ) - level.player2 GetEye() );
	base = to[ 1 ];
	for ( i = 0; i < 5; i++ )
	{
		level.player2 SetPlayerAngles( ( 0, base, 0 ) );
		level.player SetPlayerAngles( ( 0, base + i * 90, 0 ) );
		wait 3;
		report( "shadowcheck: step " + i + ", player 1 yaw " + level.player GetPlayerAngles()[ 1 ] + ", player 2 yaw " + level.player2 GetPlayerAngles()[ 1 ] );
		SetDvar( "mw2coop_qa_frame", "sh-" + i );
		wait 1.5;
	}
	report( "done" );
}

wait_intro()
{
	if ( !flag_exist( "introscreen_complete" ) || !flag( "introscreen_complete" ) )
		level waittill_notify_or_timeout( "introscreen_complete", 15 );
	wait 0.5;
}

// True when no 8-unit step is free in any direction: he stands inside something solid.
qa_stuck( player )
{
	return qa_spot_stuck( player.origin );
}

qa_spot_stuck( at )
{
	foreach ( yaw in strtok( "0 90 180 270", " " ) )
	{
		end = at + AnglesToForward( ( 0, int( yaw ), 0 ) ) * 8 + ( 0, 0, 2 );
		if ( Distance( PlayerPhysicsTrace( at + ( 0, 0, 2 ), end ), end ) < 0.5 )
			return false;
	}
	return true;
}

player_name( player )
{
	if ( player == level.player2 )
		return "player 2";
	return "player 1";
}

// Real input for a bot: the machine of that player presses the button as a key bound to it would
// ("activate" for +activate, "forward", "gostand", ...) for ms milliseconds, in steps of 250 ms
// (mw2coop_bind in the DLL, src/control.c). Both machines run this; only his acts on it. The DLL
// reads the request every 250 ms, hence the wait.
press( player, button, ms )
{
	if ( !isdefined( level.mw2coop_qa_presses ) )
		level.mw2coop_qa_presses = 0;
	level.mw2coop_qa_presses++;
	n = 1;
	if ( player == level.player2 )
		n = 2;
	SetDvar( "mw2coop_qa_press", n + " " + button + " " + ms + " " + level.mw2coop_qa_presses );
	wait 0.3;
}

// --------------------------------------------------------------------------------------------
// Real input (any map): each player's own machine presses Forward, then Use (press()). He walks
// forward, and the server sees his Use button held, not the other player's.
// split screen (settings.ini [splitscreen], mw2coop\fakepad.txt): player 2's copy is driven by a pad (a QA pad file holds the
// stick forward and turned); he walks and his view turns, player 1, driven by nobody, stays
padwalk()
{
	check( level.players.size == 2 && isdefined( level.player2 ), "two players (" + level.players.size + ")" );
	level.mw2coop_no_follow = true;
	wait_intro();
	wait 6;
	from = [];
	yaw = [];
	foreach ( n, player in level.players )
	{
		from[ n ] = player.origin;
		yaw[ n ] = player GetPlayerAngles()[ 1 ];
	}
	wait 5;
	foreach ( n, player in level.players )
	{
		moved = Distance2D( player.origin, from[ n ] );
		turned = AngleClamp180( player GetPlayerAngles()[ 1 ] - yaw[ n ] );
		report( "padwalk: " + player_name( player ) + " moved " + int( moved ) + ", turned " + int( turned ) );
	}
	// control: a Forward press through the game's own binding moves him, whoever drives the copy
	foreach ( n, player in level.players )
	{
		from[ n ] = player.origin;
		press( player, "forward", 1000 );
	}
	wait 2;
	foreach ( n, player in level.players )
		report( "padwalk control: " + player_name( player ) + " moved " + int( Distance2D( player.origin, from[ n ] ) ) );
	wait 1;
	report( "done" );
}

buttons()
{
	check( level.players.size == 2 && isdefined( level.player2 ), "two players (" + level.players.size + ")" );
	level.mw2coop_no_follow = true;
	wait_intro();
	wait 2;
	foreach ( n, player in level.players )
	{
		other = level.players[ 1 - n ];
		check( !qa_stuck( player ), player_name( player ) + " is not stuck in anything solid (at " + vec_int( player.origin ) + ")" );
		// face the way with the most free floor (up to 100 units; the range's bays are narrow)
		yaw = player GetPlayerAngles()[ 1 ];
		room = 0;
		best = yaw;
		for ( turn = 0; turn < 360; turn += 45 )
		{
			from = player.origin + ( 0, 0, 8 );
			end = from + AnglesToForward( ( 0, yaw + turn, 0 ) ) * 100;
			free = Distance( from, PlayerPhysicsTrace( from, end ) );
			if ( free > room + 1 )
			{
				room = free;
				best = yaw + turn;
			}
		}
		yaw = best;
		need = min( 40, room * 0.5 );
		player SetPlayerAngles( ( 0, yaw, 0 ) );
		wait 0.2;
		// a level may hold a player still for a while: try again a few times
		moved = 0;
		other_moved = 0;
		for ( try = 1; try <= 5 && moved <= need; try++ )
		{
			if ( try > 1 )
				wait 3;
			from = player.origin;
			other_from = other.origin;
			press( player, "forward", 1000 );
			wait 1.5;
			moved = Distance2D( player.origin, from );
			other_moved = Distance2D( other.origin, other_from );
		}
		check( moved > need, player_name( player ) + " walks forward on a Forward press (" + int( moved ) + " of " + int( room ) + " free units, press " + ( try - 1 ) + ")" );
		check( other_moved < 8, "the other player stays (" + int( other_moved ) + " units)" );
		press( player, "activate", 1000 );
		held = false;
		other_held = false;
		for ( t = 0; t < 1.5 && !held; t += 0.05 )
		{
			held = player UseButtonPressed();
			other_held = other UseButtonPressed();
			wait 0.05;
		}
		check( held, "the server sees " + player_name( player ) + "'s Use button (" + t + " s)" );
		check( !other_held, "and not the other player's" );
		wait 1;
	}
	report( "done" );
}

vec_int( v )
{
	return int( v[ 0 ] ) + " " + int( v[ 1 ] ) + " " + int( v[ 2 ] );
}

// --------------------------------------------------------------------------------------------
// Who the enemies fight (any map, a start point with a fight): over 45 s, every 0.5 s, whose
// player each axis AI has as its enemy, and the hits each player takes from axis AI.
targets()
{
	check( level.players.size == 2 && isdefined( level.player2 ), "two players (" + level.players.size + ")" );
	foreach ( player in level.players )
	{
		player.mw2coop_qa_hits = 0;
		player thread count_hits();
	}
	on1 = 0;
	on2 = 0;
	other = 0;
	samples = 0;
	most = 0;
	for ( t = 0; t < 45; t += 0.5 )
	{
		ai = GetAIArray( "axis" );
		most = max( most, ai.size );
		foreach ( guy in ai )
		{
			if ( !isdefined( guy.enemy ) )
				continue;
			samples++;
			if ( guy.enemy == level.player )
				on1++;
			else if ( guy.enemy == level.player2 )
				on2++;
			else
				other++;
		}
		wait 0.5;
	}
	report( "enemy samples " + samples + " (up to " + most + " axis AI): on player 1 " + on1 + ", on player 2 " + on2 + ", on allies " + other );
	report( "hits from axis AI: player 1 " + level.player.mw2coop_qa_hits + ", player 2 " + level.player2.mw2coop_qa_hits + "; players " + int( Distance( level.player.origin, level.player2.origin ) ) + " apart" );
	check( on2 > 0, "enemies take player 2 as their enemy too" );
	check( level.player2.mw2coop_qa_hits > 0 || on2 == 0, "and hit him" );
	report( "done" );
}

count_hits()
{
	self endon( "death" );
	for ( ;; )
	{
		self waittill( "damage", amount, attacker );
		if ( isdefined( attacker ) && IsAI( attacker ) && attacker.team == "axis" )
			self.mw2coop_qa_hits++;
	}
}

// --------------------------------------------------------------------------------------------
// Bodies check (any map): the players stand 120 units apart facing each other for 20 s, for
// screenshots of each one's body on the other's screen.
look()
{
	check( level.players.size == 2 && isdefined( level.player2 ), "two players (" + level.players.size + ")" );
	level.mw2coop_no_follow = true;
	wait 3;
	fwd = AnglesToForward( ( 0, level.player GetPlayerAngles()[ 1 ], 0 ) );
	spot = PlayerPhysicsTrace( level.player.origin + fwd * 120 + ( 0, 0, 48 ), level.player.origin + fwd * 120 - ( 0, 0, 96 ) );
	level.player2 SetOrigin( spot );
	// each machine saves its frame of the other player (src/shot.c): standing, then player 2
	// walking towards player 1 on a real Forward press (his body's walk animation)
	for ( i = 0; i < 200; i++ )
	{
		level.player SetPlayerAngles( VectorToAngles( level.player2 GetEye() - ( 0, 0, 20 ) - level.player GetEye() ) );
		level.player2 SetPlayerAngles( VectorToAngles( level.player GetEye() - ( 0, 0, 20 ) - level.player2 GetEye() ) );
		if ( i == 20 )
			report( "facing each other: player 2 model " + level.player2.model + ", player 1 model " + level.player.model );
		if ( i == 30 )
			thread qa_shot( "look-standing" );
		if ( i == 60 )
			thread press( level.player2, "forward", 1000 );
		if ( i == 68 )
			thread qa_shot( "look-walking" );
		wait 0.1;
	}
	report( "done" );
}

// No Russian's cover (airport, start "stairs"): player 2's shots at Makarov's squad blow it (the
// level only checks IsPlayer( attacker )); Makarov then shoots both and the mission fails for
// both (maps\_mw2coop_airport::makarov_kills_players). Names no airport script.
cover()
{
	check( level.players.size == 2 && isdefined( level.player2 ), "two players (" + level.players.size + ")" );
	for ( t = 0; t < 30 && !isdefined( level.makarov ); t += 0.5 )
		wait 0.5;
	check( isdefined( level.makarov ), "Makarov there" );
	wait 3;
	for ( i = 0; i < 6 && !flag( "friendly_fire_warning" ); i++ )
	{
		level.makarov DoDamage( 1, level.makarov.origin, level.player2 );
		wait 0.5;
	}
	check( flag( "friendly_fire_warning" ), "player 2's shots at Makarov's squad blew the cover (" + i + " hits)" );
	SetDvar( "mw2coop_qa_phase", "done" );	// the failure restarts the level
	for ( t = 0; t < 40 && !level.mw2coop.failing; t += 0.5 )
		wait 0.5;
	check( level.mw2coop.failing, "Makarov's squad turned on them and the mission failed for both (" + t + " s)" );
	report( "done" );
}

// Each machine saves frames of the other player's whole figure: standing, player 2 stepping to
// his right on a real press, backing off (src/shot.c). For body models (No Russian's transplant).
bodyview()
{
	check( level.players.size == 2 && isdefined( level.player2 ), "two players (" + level.players.size + ")" );
	level.mw2coop_no_follow = true;
	wait 3;
	fwd = AnglesToForward( ( 0, level.player GetPlayerAngles()[ 1 ], 0 ) );
	spot = PlayerPhysicsTrace( level.player.origin + fwd * 220 + ( 0, 0, 48 ), level.player.origin + fwd * 220 - ( 0, 0, 96 ) );
	level.player2 SetOrigin( spot );
	steps = [];
	steps[ 15 ] = "stand";
	steps[ 25 ] = "+moveright";
	steps[ 33 ] = "right";
	steps[ 45 ] = "+back";
	steps[ 53 ] = "back";
	for ( i = 0; i < 80; i++ )
	{
		level.player SetPlayerAngles( VectorToAngles( level.player2.origin + ( 0, 0, 36 ) - level.player GetEye() ) );
		level.player2 SetPlayerAngles( VectorToAngles( level.player.origin + ( 0, 0, 36 ) - level.player2 GetEye() ) );
		if ( isdefined( steps[ i ] ) )
		{
			if ( GetSubStr( steps[ i ], 0, 1 ) == "+" )
				thread press( level.player2, GetSubStr( steps[ i ], 1 ), 1500 );
			else
				thread qa_shot( "body-" + steps[ i ] );
		}
		wait 0.1;
	}
	report( "done" );
}

// No Russian's lobby (airport, start "intro"; run with MW2COOP_GOD=0): the squad kills whoever
// walks into its line of fire before it moves on (maps\_mw2coop_airport::line_of_fire_kill); the
// level used to kill player 1 whoever stepped in. Names no airport script.
lineoffire()
{
	check( level.players.size == 2 && isdefined( level.player2 ), "two players (" + level.players.size + ")" );
	for ( t = 0; t < 60 && !flag( "lobby_open_fire" ); t += 0.5 )
		wait 0.5;
	check( flag( "lobby_open_fire" ), "the squad opens fire in the lobby (" + t + " s)" );
	trig = GetEnt( "intro_line_of_fire_trig_final", "targetname" );
	check( isdefined( trig ) && !flag( "lobby_to_stairs_go" ), "the line of fire is live" );
	SetDvar( "mw2coop_qa_phase", "done" );	// the death restarts the level
	level.player2 SetOrigin( trig.origin );
	for ( t = 0; t < 5 && IsAlive( level.player2 ) && !level.mw2coop.failing; t += 0.1 )
		wait 0.1;
	check( !IsAlive( level.player2 ) || level.mw2coop.failing, "player 2 died in the line of fire (" + t + " s)" );
	check( IsAlive( level.player ), "player 1 still alive" );
	check( isdefined( level.mw2coop_lof_victim ) && level.mw2coop_lof_victim == level.player2, "the squad's line of fire killed player 2, the one who walked in" );
	for ( t = 0; t < 10 && !level.mw2coop.failing; t += 0.5 )
		wait 0.5;
	check( level.mw2coop.failing, "the mission failed for both" );
	report( "done" );
}

// Aim assist of player 2's pad (see the list at the top). Each phase's samples go out in one
// report: report() waits, and the DLL reads the log dvar every 250 ms.
aimassist()
{
	check( level.players.size == 2 && isdefined( level.player2 ), "two players (" + level.players.size + ")" );
	level.mw2coop_no_follow = true;
	if ( !flag_exist( "introscreen_complete" ) || !flag( "introscreen_complete" ) )
		level waittill_notify_or_timeout( "introscreen_complete", 30 );
	wait 8;	// the title lines are gone
	p2 = level.player2;
	// the allies are drawn (Price was not, with the aim target check writing every entity's flags)
	allies = GetAIArray( "allies" );
	text = allies.size + " allies:";
	foreach ( ally in allies )
		text = text + " " + ally GetEntityNumber() + " " + ally.model + " " + int( Distance( ally.origin, level.player.origin ) );
	report( text );
	if ( allies.size )
	{
		ally = allies[ 0 ];
		foreach ( other in allies )
			if ( DistanceSquared( other.origin, level.player.origin ) < DistanceSquared( ally.origin, level.player.origin ) )
				ally = other;
		for ( t = 0; t < 10; t++ )
		{
			foreach ( player in level.players )
				player SetPlayerAngles( VectorToAngles( ally GetEye() - player GetEye() ) );
			wait 0.05;
		}
		SetDvar( "mw2coop_qa_frame", "aa-ally" );
		wait 0.5;
	}
	guy = aa_enemy( p2 );
	check( isdefined( guy ), "an enemy to aim at" );
	if ( !isdefined( guy ) )
		return;
	foreach ( player in level.players )
		player.ignoreme = true;
	guy.ignoreall = true;
	guy.goalradius = 8;
	yaw = p2 GetPlayerAngles()[ 1 ];
	ahead = p2.origin + AnglesToForward( ( 0, yaw, 0 ) ) * 400;
	spot = PlayerPhysicsTrace( ahead + ( 0, 0, 48 ), ahead - ( 0, 0, 96 ) );
	guy ForceTeleport( spot, ( 0, yaw + 180, 0 ) );
	guy SetGoalPos( spot );
	guy thread aa_hold( spot, yaw + 180 );
	start = p2.origin;
	wait 1;
	report( "enemy " + guy GetEntityNumber() + " " + int( Distance( p2.origin, guy.origin ) ) + " units ahead" );

	// 1: aiming down the sight 6 degrees beside it
	aa_aim( p2, guy, 6 );
	wait 0.5;
	text = "snap: before " + aa_off( p2, guy );
	SetDvar( "mw2coop_qa_pad2", "0 0 0 0 0 255 0" );
	for ( i = 1; i <= 8; i++ )
	{
		wait 0.1;
		text = text + ", " + ( i * 100 ) + " ms " + aa_off( p2, guy );
		if ( i == 6 )
			SetDvar( "mw2coop_qa_frame", "aa-ads" );
	}
	off = aa_yaw_off( p2, guy );
	report( text );
	check( abs( off ) < 2, "aiming down the sight turned player 2 onto the enemy (" + off + " degrees off)" );
	SetDvar( "mw2coop_qa_pad2", "0 0 0 0 0 0 0" );
	wait 1;

	// 2: a steady turn to the right from 25 degrees to the left of it
	aa_aim( p2, guy, 25 );
	wait 0.5;
	SetDvar( "mw2coop_qa_pad2", "0 0 16000 0 0 0 0" );
	text = "turn (off, degrees a second):";
	far = 0;
	over = 0;
	far_n = 0;
	over_n = 0;
	last = p2 GetPlayerAngles()[ 1 ];
	for ( i = 0; i < 24; i++ )
	{
		wait 0.05;
		now = p2 GetPlayerAngles()[ 1 ];
		speed = AngleClamp180( last - now ) / 0.05;
		last = now;
		off = aa_yaw_off( p2, guy );
		text = text + " " + int( off ) + "/" + int( speed );
		if ( abs( off ) < 2 )
		{
			over += speed;
			over_n++;
		}
		else if ( abs( off ) > 10 )
		{
			far += speed;
			far_n++;
		}
	}
	SetDvar( "mw2coop_qa_pad2", "0 0 0 0 0 0 0" );
	report( text );
	if ( far_n && over_n )
		check( over / over_n < 0.8 * far / far_n, "the turn slows over the enemy (" + int( far / far_n ) + " away, " + int( over / over_n ) + " over it)" );
	else
		report( "FAIL the turn did not pass over the enemy (" + far_n + " samples away, " + over_n + " over it)" );
	wait 1;

	// 3: strafing right for a second while looking at it, with aim assist and without (the pad
	// dvar's eighth value turns it off: a script cannot write the option's code dvar)
	result = [];
	for ( assist = 1; assist >= 0; assist-- )
	{
		p2 SetOrigin( start );
		aa_aim( p2, guy, 0 );
		wait 0.5;
		SetDvar( "mw2coop_qa_pad2", "32767 0 0 0 0 0 0 " + ( 1 - assist ) );
		wait 0.5;
		if ( assist )
			SetDvar( "mw2coop_qa_frame", "aa-strafe" );
		wait 0.5;
		result[ assist ] = aa_yaw_off( p2, guy );
		SetDvar( "mw2coop_qa_pad2", "0 0 0 0 0 0 0" );
		wait 0.5;
	}
	level notify( "mw2coop_aa_done" );
	check( abs( result[ 1 ] ) < abs( result[ 0 ] ) * 0.8, "a strafe keeps nearer the enemy with aim assist (" + result[ 1 ] + " degrees off, without " + result[ 0 ] + ")" );
	// 4: vibration ([native] rumblelog=1 logs it): damage on player 2, a grenade near him, one far off
	p2 PlayRumbleOnEntity( "damage_heavy" );
	wait 1;
	PlayRumbleOnPosition( "grenade_rumble", p2.origin + ( 200, 0, 0 ) );
	wait 1;
	PlayRumbleOnPosition( "grenade_rumble", p2.origin + ( 5000, 0, 0 ) );
	wait 1;
	report( "rumbles played: damage on player 2, a grenade 200 units off, one 5000 units off" );
	report( "done" );
}

// keeps an AI at one spot facing one way (a patrol would walk it off)
aa_hold( spot, yaw )
{
	self endon( "death" );
	level endon( "mw2coop_aa_done" );
	for ( ;; )
	{
		self ForceTeleport( spot, ( 0, yaw, 0 ) );
		wait 0.05;
	}
}

// the axis AI nearest p, or one spawned from an axis spawner
aa_enemy( p )
{
	best = undefined;
	foreach ( guy in GetAIArray( "axis" ) )
		if ( !isdefined( best ) || DistanceSquared( guy.origin, p.origin ) < DistanceSquared( best.origin, p.origin ) )
			best = guy;
	if ( isdefined( best ) )
		return best;
	foreach ( spawner in GetSpawnerTeamArray( "axis" ) )
	{
		guy = spawner StalingradSpawn();
		if ( !spawn_failed( guy ) )
			return guy;
	}
	return undefined;
}

aa_aim( p, guy, yaw_off )
{
	ang = VectorToAngles( guy GetTagOrigin( "j_head" ) - p GetEye() );
	p SetPlayerAngles( ( ang[ 0 ], ang[ 1 ] + yaw_off, 0 ) );
}

aa_yaw_off( p, guy )
{
	return AngleClamp180( p GetPlayerAngles()[ 1 ] - VectorToAngles( guy GetTagOrigin( "j_head" ) - p GetEye() )[ 1 ] );
}

aa_off( p, guy )
{
	ang = VectorToAngles( guy GetTagOrigin( "j_head" ) - p GetEye() );
	now = p GetPlayerAngles();
	return int( AngleClamp180( now[ 1 ] - ang[ 1 ] ) * 10 ) / 10 + "/" + int( AngleClamp180( now[ 0 ] - ang[ 0 ] ) * 10 ) / 10;
}

// Player 2's body faces where his camera looks (see the list at the top; the owner saw it turned
// away from the camera on Contingency, 2026-10-28)
bodycam()
{
	check( level.players.size == 2 && isdefined( level.player2 ), "two players (" + level.players.size + ")" );
	level.mw2coop_no_follow = true;
	if ( !flag_exist( "introscreen_complete" ) || !flag( "introscreen_complete" ) )
		level waittill_notify_or_timeout( "introscreen_complete", 30 );
	wait 2;
	p1 = level.player;
	p2 = level.player2;
	yaw = p1 GetPlayerAngles()[ 1 ];
	ahead = p1.origin + AnglesToForward( ( 0, yaw, 0 ) ) * 150;
	p2 SetOrigin( PlayerPhysicsTrace( ahead + ( 0, 0, 48 ), ahead - ( 0, 0, 96 ) ) );
	for ( i = 0; i < 4; i++ )
	{
		p2 SetPlayerAngles( ( 0, yaw + 180 + i * 90, 0 ) );	// 0: facing player 1
		for ( t = 0; t < 20; t++ )
		{
			p1 SetPlayerAngles( VectorToAngles( p2 GetEye() - ( 0, 0, 20 ) - p1 GetEye() ) );
			wait 0.05;
		}
		SetDvar( "mw2coop_qa_frame", "bodycam-" + ( i * 90 ) );
		report( "player 2 turned " + ( i * 90 ) + " from facing player 1: view yaw " + int( p2 GetPlayerAngles()[ 1 ] ) + ", body yaw " + int( p2.angles[ 1 ] ) + ", player 1 yaw " + int( yaw ) );
	}
	SetDvar( "mw2coop_qa_pad2", "0 0 20000 0 0 0 0" );
	for ( t = 0; t < 20; t++ )
	{
		p1 SetPlayerAngles( VectorToAngles( p2 GetEye() - ( 0, 0, 20 ) - p1 GetEye() ) );
		wait 0.05;
	}
	SetDvar( "mw2coop_qa_pad2", "0 0 0 0 0 0 0" );
	wait 0.5;
	SetDvar( "mw2coop_qa_frame", "bodycam-pad" );
	report( "player 2 turned by his pad: view yaw " + int( p2 GetPlayerAngles()[ 1 ] ) + ", body yaw " + int( p2.angles[ 1 ] ) );
	report( "done" );
}

// a hint naming a key for each player (see the list at the top)
glyphs()
{
	check( level.players.size == 2 && isdefined( level.player2 ), "two players (" + level.players.size + ")" );
	if ( !flag_exist( "introscreen_complete" ) || !flag( "introscreen_complete" ) )
		level waittill_notify_or_timeout( "introscreen_complete", 30 );
	wait 3;
	foreach ( player in level.players )
	{
		hint = NewClientHudElem( player );
		hint.x = 0;
		hint.y = 60;
		hint.alignX = "center";
		hint.alignY = "middle";
		hint.horzAlign = "center";
		hint.vertAlign = "middle";
		hint.fontScale = 1.6;
		hint.font = "default";
		hint SetText( &"TRAINER_HINT_STAND" );
	}
	wait 2;
	SetDvar( "mw2coop_qa_frame", "glyphs" );
	wait 1;
	report( "done" );
}

// hudlayout: (native split screen) both players with the level's weapons and HUD, then a nearly empty clip, so each half shows the ammo, the
// grenades, the compass, the d-pad, the stance and the low ammo warning (use a level whose script shows the HUD: not the trainer); composed frames "hud-a" (standing) and "hud-b" (crouched, low clip)
hudlayout()
{
	check( level.players.size == 2 && isdefined( level.player2 ), "two players (" + level.players.size + ")" );
	if ( !flag_exist( "introscreen_complete" ) || !flag( "introscreen_complete" ) )
		level waittill_notify_or_timeout( "introscreen_complete", 30 );
	wait 2;
	SetSavedDvar( "compass", 1 );
	SetSavedDvar( "ammoCounterHide", 0 );
	SetSavedDvar( "hud_showStance", 1 );
	SetSavedDvar( "actionSlotsHide", 0 );
	foreach ( player in level.players )
		if ( player GetCurrentWeapon() == "none" )	// (a level without a starting weapon: the trainer's rifle)
		{
			player GiveWeapon( "m4_grunt" );
			player SwitchToWeapon( "m4_grunt" );
		}
	wait 3;
	SetDvar( "mw2coop_qa_frame", "hud-a" );
	wait 1;
	foreach ( player in level.players )
	{
		if ( player GetCurrentWeapon() != "none" )
			player SetWeaponAmmoClip( player GetCurrentWeapon(), 2 );
		player SetStance( "crouch" );
	}
	wait 2;
	SetDvar( "mw2coop_qa_frame", "hud-b" );
	wait 1;
	report( "player 1 holds " + level.player GetCurrentWeapon() + ", player 2 " + level.player2 GetCurrentWeapon() );
	wait 3;	// (with [native] qa_ads / qa_ads1 a player aims down the sights by now: the scope with the low ammo warning on)
	SetDvar( "mw2coop_qa_frame", "hud-c" );
	wait 2;
	SetDvar( "mw2coop_qa_frame", "hud-d" );
	wait 1;
}

// both players armed and facing each other 150 units apart ([native] entdump=1 logs the entity
// slots: which fields carry a body's weapon); the composed picture is saved as "armed"
armed()
{
	check( level.players.size == 2 && isdefined( level.player2 ), "two players (" + level.players.size + ")" );
	if ( !flag_exist( "introscreen_complete" ) || !flag( "introscreen_complete" ) )
		level waittill_notify_or_timeout( "introscreen_complete", 30 );
	wait 2;
	foreach ( player in level.players )
		if ( player GetCurrentWeapon() == "none" )
		{
			player GiveWeapon( "m4_grunt" );
			player SwitchToWeapon( "m4_grunt" );
		}
	wait 1;
	p1 = level.player;
	p2 = level.player2;
	p1 thread armed_watch( "player 1" );
	p2 thread armed_watch( "player 2" );
	yaw = p1 GetPlayerAngles()[ 1 ] + GetDvarInt( "mw2coop_qa_yawoff" );	// where player 2 stands, seen from player 1 (try another side when something stands between)
	ahead = p1.origin + AnglesToForward( ( 0, yaw, 0 ) ) * 150;
	p2 SetOrigin( PlayerPhysicsTrace( ahead + ( 0, 0, 48 ), ahead - ( 0, 0, 96 ) ) );
	for ( t = 0; t < 160; t++ )
	{
		p1 SetPlayerAngles( VectorToAngles( p2 GetEye() - ( 0, 0, 20 ) - p1 GetEye() ) );
		p2 SetPlayerAngles( VectorToAngles( p1 GetEye() - ( 0, 0, 20 ) - p2 GetEye() ) );
		if ( t == 100 )
			SetDvar( "mw2coop_qa_frame", "armed" );
		wait 0.05;
	}
	report( "player 1 holds " + p1 GetCurrentWeapon() + ", player 2 " + p2 GetCurrentWeapon() );
	// player 2 changes to a pistol first: his body in player 1's half (the top one)
	p2 GiveWeapon( "usp" );
	p2 SwitchToWeapon( "usp" );
	for ( t = 0; t < 80; t++ )
	{
		p1 SetPlayerAngles( VectorToAngles( p2 GetEye() - ( 0, 0, 20 ) - p1 GetEye() ) );
		p2 SetPlayerAngles( VectorToAngles( p1 GetEye() - ( 0, 0, 20 ) - p2 GetEye() ) );
		if ( t == 60 )
			SetDvar( "mw2coop_qa_frame", "armed-p2usp" );
		wait 0.05;
	}
	report( "player 2 changed to " + p2 GetCurrentWeapon() );
	// player 1 changes to a pistol: his body in player 2's half must hold it (and the rifle again after)
	p1 GiveWeapon( "usp" );
	for ( round = 0; round < 2; round++ )
	{
		if ( round == 0 )
			p1 SwitchToWeapon( "usp" );
		else
			p1 SwitchToWeapon( level.motiontrackergun_off );
		for ( t = 0; t < 80; t++ )
		{
			p1 SetPlayerAngles( VectorToAngles( p2 GetEye() - ( 0, 0, 20 ) - p1 GetEye() ) );
			p2 SetPlayerAngles( VectorToAngles( p1 GetEye() - ( 0, 0, 20 ) - p2 GetEye() ) );
			if ( t == 60 )
				SetDvar( "mw2coop_qa_frame", "armed-switched" + round );
			wait 0.05;
		}
		text = "player 1 changed to " + p1 GetCurrentWeapon() + "; his attached models:";
		for ( i = 0; i < p1 GetAttachSize(); i++ )
			text = text + " " + p1 GetAttachModelName( i ) + "@" + p1 GetAttachTagName( i );
		report( text + " (model " + p1.model + ")" );
	}
	report( "done" );
}

armed_watch( who )
{
	for ( ;; )
	{
		self waittill( "weaponchange" );
		SetDvar( "mw2coop_qa_log", "weaponchange " + who + ": " + self GetCurrentWeapon() + ", model " + self GetPlayerWeaponModel() );
		wait 0.3;
	}
}

// the level's game time into a dvar every frame, which [native] clocklog=1 compares with the wall clock once a second
clock()
{
	for ( ;; )
	{
		SetDvar( "mw2coop_qa_t", GetTime() );
		wait 0.05;
	}
}

// the nearest ally's real speed: its path length and playback rate once a second, into the dvar mw2coop_qa_s
// ("<entity number> <units> <playback rate>"), which [native] speedlog=1 sets against what is drawn
speed()
{
	if ( !flag_exist( "introscreen_complete" ) || !flag( "introscreen_complete" ) )
		level waittill_notify_or_timeout( "introscreen_complete", 30 );
	wait 3;
	for ( ;; )
	{
		best = undefined;
		foreach ( ally in GetAIArray( "allies" ) )
			if ( !isdefined( best ) || DistanceSquared( ally.origin, level.player.origin ) < DistanceSquared( best.origin, level.player.origin ) )
				best = ally;
		if ( !isdefined( best ) )
		{
			wait 1;
			continue;
		}
		len = 0;
		last = best.origin;
		for ( i = 0; i < 20 && isalive( best ); i++ )
		{
			wait 0.05;
			len += Distance( best.origin, last );
			last = best.origin;
		}
		rate = 1;
		if ( isdefined( best.moveplaybackrate ) )
			rate = best.moveplaybackrate;
		SetDvar( "mw2coop_qa_s", best GetEntityNumber() + " " + int( len ) + " " + rate );
	}
}

// Cliffhanger climb in SINGLE PLAYER (mw2coop_qa climbsolo, run with MW2COOP_PLAIN=1; thread started by
// maps\_mw2coop::main without a co-op session). Vlad, 2026-11-07: weapon given at the start, no death from
// the fall, no scream. After the cave scene the player must hold only the ice picks; the fall (fade_to_death)
// must run the whole death_trigger: black, then the mission fails ("ui_deadquote" set) or the player is killed.
climbsolo()
{
	level.mw2coop_qa_n = 0;
	while ( !flag_exist( "flyin_complete" ) )
		wait 0.05;
	// the cave scene (teleport_to_cave) sets the flag; if it never does, look anyway after 150 s
	for ( i = 0; i < 150 && !flag( "flyin_complete" ); i++ )
		wait 1;
	report( "solo: flyin_complete " + flag( "flyin_complete" ) + " after " + i + " s" );
	wait 4;
	p = level.player;
	list = p GetWeaponsListAll();
	text = "";
	foreach ( w in list )
		text += w + " ";
	report( "solo: current '" + p GetCurrentWeapon() + "' weapons [" + text + "] at " + vec_int( p.origin ) );
	check( list.size == 1 && list[ 0 ] == level.ice_pick_viewweapon, "only the ice picks in hand at the start of the climb" );
	SetDvar( "ui_deadquote", "" );
	report( "solo: the fall: fade_to_death set" );
	flag_set( "fade_to_death" );
	alive = true;
	for ( i = 0; i < 16; i++ )
	{
		wait 1;
		if ( !IsAlive( p ) )
			alive = false;
		report( "solo: t " + ( i + 1 ) + " alive " + IsAlive( p ) + " deadquote '" + GetDvar( "ui_deadquote" ) + "'" );
		if ( !alive || GetDvar( "ui_deadquote" ) != "" )
			break;
	}
	check( !alive || GetDvar( "ui_deadquote" ) != "", "the fall killed the player or failed the mission" );
	report( "done" );
}

// fxplay: (split screen) an effect a SCRIPT plays (PlayFX, as the levels' exploders and ambient smoke do), in front of both players: player 2 stands beside player 1 and looks
// the same way, the level's first smoke effect (else its first effect) is played 300 units ahead every second for 8 s, with frames asked for (owner: "when the first player
// sees fog, the other one doesn't").
fxplay()
{
	wait_intro();
	wait 4;
	level.mw2coop_no_follow = true;
	level.mw2coop_no_leash = true;
	name = undefined;
	foreach ( n, fx in level._effect )
	{
		if ( !isdefined( name ) )
			name = n;
		if ( IsSubStr( n, "smoke" ) )
		{
			name = n;
			break;
		}
	}
	if ( !isdefined( name ) )
	{
		report( "fxplay: the level has no effects" );
		return;
	}
	yaw = level.player GetPlayerAngles()[ 1 ];
	level.player2 SetOrigin( level.player.origin + AnglesToRight( ( 0, yaw, 0 ) ) * 40 );
	level.player2 SetPlayerAngles( ( 0, yaw, 0 ) );
	spot = level.player.origin + AnglesToForward( ( 0, yaw, 0 ) ) * 300 + ( 0, 0, 30 );
	report( "fxplay: '" + name + "' at " + vec_int( spot ) );
	for ( i = 0; i < 8; i++ )
	{
		PlayFX( level._effect[ name ], spot );
		wait 1;
		if ( i == 2 || i == 5 )
			SetDvar( "mw2coop_qa_frame", "fxplay-" + i ); // (the composed frame: both halves, the smoke in each?)
	}
	report( "fxplay done" );
}

// predsteer: (contingency, start "ridge", split screen with player 2 on a pad: "p1pad=0") player 2 raises the Predator laptop (d-pad right), fires (right trigger) and holds the
// right stick to one side for 3 s: does the missile turn? (Nexus report: "you can't move the Predator to aim with a pad, it reads the mouse only")
predsteer()
{
	level.mw2coop_no_follow = true;
	// (Contingency: the laptop comes when Price is on the ridge and the stealth enemies are gone: both are made so)
	wait 8;
	foreach ( ai in GetAISpeciesArray( "axis", "all" ) )
		if ( IsAlive( ai ) )
			ai Kill();
	// (handle_ridge waits for approaching_ridge, first_uav_sequence for player_on_ridge then price_on_ridge: triggers the players would walk into; start ridge)
	flags = [];
	flags[ 0 ] = "approaching_ridge";
	flags[ 1 ] = "player_on_ridge";
	flags[ 2 ] = "price_on_ridge";
	foreach ( f in flags )
	{
		if ( flag_exist( f ) )
			flag_set( f );
		wait 1;
	}
	t = 0;
	pl = undefined;
	while ( !isdefined( pl ) && t < 240 )
	{
		foreach ( player in level.players )
			if ( player HasWeapon( "remote_missile_detonator" ) )
				pl = player;
		if ( GetDvar( "mw2coop_qa_who" ) == "1" && level.player HasWeapon( "remote_missile_detonator" ) ) // (the laptop's holder: the last one, or player 1 with mw2coop_qa_who 1)
			pl = level.player;
		wait 1;
		t++;
	}
	if ( !isdefined( pl ) )
	{
		report( "nobody has the laptop after " + t + " s" );
		return;
	}
	pad = "mw2coop_qa_pad1";
	if ( pl == level.player2 )
		pad = "mw2coop_qa_pad2";
	report( "the laptop: " + pad + " after " + t + " s" );
	wait 4;
	SetDvar( pad, "0 0 0 0 8 0 0 0" );
	wait 0.4;
	SetDvar( pad, "0 0 0 0 0 0 0 0" );
	wait 3;
	report( "weapon " + pl GetCurrentWeapon() );
	SetDvar( pad, "0 0 0 0 0 0 255 0" );
	wait 0.4;
	SetDvar( pad, "0 0 0 0 0 0 0 0" );
	for ( i = 0; i < 30 && !isdefined( level.remoteMissile ); i++ )
		wait 0.1;
	if ( !isdefined( level.remoteMissile ) )
	{
		report( "no missile in flight" );
		return;
	}
	wait 1.2;
	m = level.remoteMissile;
	report( "missile " + vec_int( m.origin ) + " angles " + vec_int( m.angles ) );
	SetDvar( pad, "0 0 25000 0 0 0 0 0" );
	for ( i = 0; i < 6 && isdefined( m ); i++ )
	{
		wait 0.5;
		if ( isdefined( m ) )
			report( "stick right " + i + ": missile angles " + vec_int( m.angles ) );
	}
	SetDvar( pad, "0 0 0 0 0 0 0 0" );
	report( "done" );
}

// Exodus (arcadia): the laser designator with a pad (Nexus report, 2026-11-16: "it says to press the right pad button, but when I press it nothing happens"). The holder's pad
// presses d-pad right (laser on), then the right trigger (designate), then d-pad right again (off); the level's NotifyOnPlayerCommand notifies are reported. Both pads.
laserpad()
{
	level.mw2coop_no_follow = true;
	wait_intro();
	wait 4;
	pl = maps\_mw2coop_arcadia::laser_player();
	other = level.player;
	pad = "mw2coop_qa_pad1";
	if ( pl == level.player2 )
		pad = "mw2coop_qa_pad2";
	else
		other = level.player2;
	report( "laser holder " + player_name( pl ) + " (" + pad + "), laserForceOn " + pl.laserForceOn );
	pl thread laserpad_watch( "holder" );
	if ( isdefined( other ) )
		other thread laserpad_watch( "other" );
	SetDvar( pad, "0 0 0 0 8 0 0 0" );
	wait 0.4;
	SetDvar( pad, "0 0 0 0 0 0 0 0" );
	wait 1.5;
	report( "after d-pad right: laserForceOn " + pl.laserForceOn + ", weapon " + pl GetCurrentWeapon() );
	SetDvar( pad, "0 0 0 0 0 0 255 0" );
	wait 0.4;
	SetDvar( pad, "0 0 0 0 0 0 0 0" );
	wait 1.5;
	report( "after the trigger: laserForceOn " + pl.laserForceOn );
	SetDvar( pad, "0 0 0 0 8 0 0 0" );
	wait 0.4;
	SetDvar( pad, "0 0 0 0 0 0 0 0" );
	wait 1.5;
	report( "after d-pad right again: laserForceOn " + pl.laserForceOn );
	report( "done" );
}

laserpad_watch( who )
{
	self endon( "death" );
	for ( ;; )
	{
		msg = self waittill_any_return( "use_laser", "fired_laser" );
		report( "notify " + msg + " on the " + who );
	}
}

// display_hint mirrored to player 2 (_utility.gsc.patch hunk 7) except for a hint that belongs to one player (maps\_mw2coop::hint_is_own). Exodus: player 1 gets
// "use_laser" (own: player 2 must not get it), then the same text under a name that is not in the list (shared: player 2 must get it).
hintown()
{
	level.mw2coop_no_follow = true;
	wait_intro();
	wait 3;
	level.player thread display_hint( "use_laser" );
	wait 2;
	report( "own hint: player 1 " + isdefined( level.player.current_global_hint ) + ", player 2 " + isdefined( level.player2.current_global_hint ) );
	level.trigger_hint_string[ "mw2coop_qa_shared" ] = level.trigger_hint_string[ "use_laser" ];
	level.trigger_hint_func[ "mw2coop_qa_shared" ] = level.trigger_hint_func[ "use_laser" ];
	flag_set( "used_laser" ); // (the laser hint's break test: both go)
	wait 3;
	flag_clear( "used_laser" );
	report( "after the break: player 1 " + isdefined( level.player.current_global_hint ) + ", player 2 " + isdefined( level.player2.current_global_hint ) );
	level.player thread display_hint( "mw2coop_qa_shared" );
	wait 2;
	report( "shared hint: player 1 " + isdefined( level.player.current_global_hint ) + ", player 2 " + isdefined( level.player2.current_global_hint ) );
	report( "done" );
}

// Player 2's own server commands in the split screen ([native] p2cmds): a vision set and a blur for player 2 only must darken and blur the bottom half and leave the top
// half as it is; the effects stay on to the end of the run (the composed frame). Contingency (its "black_bw" vision).
p2cmds()
{
	level.mw2coop_no_follow = true;
	wait_intro();
	wait 3;
	level.player2 VisionSetNakedForPlayer( "black_bw", 0.5 );
	level.player2 SetBlurForPlayer( 6, 0.5 );
	report( "player 2: vision black_bw and blur 6 set for him only" );
	wait 2;
	report( "done" );
}

// Player 2 fires on his pad for 2 s, then player 1 for 2 s (with [native] sndcount=2: each player's own gun must start its sounds in his own context, evsnd1 mutes only
// the world's copies in context 1)
p2fire()
{
	level.mw2coop_no_follow = true;
	wait_intro();
	wait 3;
	report( "player 2 fires" );
	SetDvar( "mw2coop_qa_pad2", "0 0 0 0 0 0 255 0" );
	wait 2;
	SetDvar( "mw2coop_qa_pad2", "0 0 0 0 0 0 0 0" );
	wait 2;
	report( "player 1 fires" );
	SetDvar( "mw2coop_qa_pad1", "0 0 0 0 0 0 255 0" );
	wait 2;
	SetDvar( "mw2coop_qa_pad1", "0 0 0 0 0 0 0 0" );
	wait 2;
	report( "done" );
}

// A pad that drops while a button is held ([native] audit A3): the test harness holds player 2's right trigger through mw2coop\fakepad.txt, then deletes the file (his
// pad is gone); his fire button must not stay pressed. Reports his attack button every second.
padlost()
{
	level.mw2coop_no_follow = true;
	for ( t = 0; t < 40; t++ )
	{
		report( "t " + t + ": player 2 attack " + level.player2 AttackButtonPressed() );
		wait 1;
	}
	report( "done" );
}

// Spec Ops snowmobile race (Vlad, mode 10: "camera turns do not work, strong jitter for both"): after the countdown (freezeControls false) each player's pad holds the right
// stick to the right for 1.5 s; his view yaw before / after is reported, and his view yaw every 0.1 s while he holds still (the jitter). Needs set specialops 1.
snowturn()
{
	wait 22;	// (the race starts 5.8 s after start_race; the level's intro before it)
	foreach ( n, player in level.players )
	{
		pad = "mw2coop_qa_pad" + ( n + 1 );
		a0 = player GetPlayerAngles();
		samples = "";
		for ( i = 0; i < 10; i++ )
		{
			wait 0.1;
			samples = samples + " " + int( player GetPlayerAngles()[ 1 ] * 10 ) / 10;
		}
		report( "snowturn: " + player_name( player ) + " still, yaw every 0.1 s:" + samples + ", linked " + player IsLinked() );
		SetDvar( pad, "0 0 25000 0 0 0 0 0" );
		wait 1.5;
		SetDvar( pad, "0 0 0 0 0 0 0 0" );
		a1 = player GetPlayerAngles();
		report( "snowturn: " + player_name( player ) + " stick right 1.5 s: yaw " + int( a0[ 1 ] ) + " -> " + int( a1[ 1 ] ) + " (turned " + int( AngleClamp180( a1[ 1 ] - a0[ 1 ] ) ) + ")" );
		wait 1;
	}
	report( "done" );
}

// ownswap (dc_whitehouse): each player keeps his own inventory (owner, 2026-11-16, maps\_mw2coop::sync_weapons). What a level gives player 1 reaches player 2; a swap on the
// ground (one weapon out, another in, in the same moment) stays player 1's.
ownswap()
{
	common_checks();
	// (only weapons the level has: the zone may not have others) a level takes one from player 1: player 2 loses it too
	level.player TakeWeapon( "flash_grenade" );
	wait 2;
	check( !level.player2 HasWeapon( "flash_grenade" ), "a level's take reaches player 2 (" + weapdrop_text( level.player2 ) + ")" );
	// player 1 swaps on the ground: his pistol out, the flash grenade in, in the same moment
	level.player TakeWeapon( "beretta" );
	level.player GiveWeapon( "flash_grenade" );
	wait 2;
	report( "ownswap: after player 1's swap, player 1 has " + weapdrop_text( level.player ) + "| player 2 has " + weapdrop_text( level.player2 ) );
	check( level.player2 HasWeapon( "beretta" ), "player 2 keeps his pistol" );
	check( !level.player2 HasWeapon( "flash_grenade" ), "player 1's pickup stays his" );
	report( "done" );
}
