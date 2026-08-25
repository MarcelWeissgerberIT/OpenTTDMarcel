/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file airport_movement.h Heart of the airports and their finite state machines. */

#ifndef AIRPORT_MOVEMENT_H
#define AIRPORT_MOVEMENT_H

#include "../airport.h"
#include "../newgrf_airport.h"

/**
 * State machine input struct (from external file, etc.)
 * Finite sTate mAchine --> FTA
 */
struct AirportFTAbuildup {
	uint8_t position; ///< The position that an airplane is at.
	uint8_t heading;  ///< The current orders (eg. TAKEOFF, HANGAR, ENDLANDING, etc.).
	AirportBlocks blocks;  ///< The block this position is on on the airport (st->airport.flags).
	uint8_t next;     ///< Next position from this position.
};

///////////////////////////////////////////////////////////////////////
/////*********Movement Positions on Airports********************///////

/**
 * Airport movement data creation macro.
 * @param x     X position.
 * @param y     Y position.
 * @param flags Movement flags.
 * @param dir   Direction.
 */
/** Dummy airport. */
static const AirportMovingData _airport_moving_data_dummy[] = {
	{    0,    0, {AirportMovingDataFlag::NoSpeedClamp, AirportMovingDataFlag::SlowTurn}, Direction::N },
	{    0,   96, {AirportMovingDataFlag::NoSpeedClamp, AirportMovingDataFlag::SlowTurn}, Direction::N },
	{   96,   96, {AirportMovingDataFlag::NoSpeedClamp, AirportMovingDataFlag::SlowTurn}, Direction::N },
	{   96,    0, {AirportMovingDataFlag::NoSpeedClamp, AirportMovingDataFlag::SlowTurn}, Direction::N },
};

/** Country Airfield (small) 4x3. */
static const AirportMovingData _airport_moving_data_country[22] = {
	{   53,    3, {AirportMovingDataFlag::ExactPosition},                                 Direction::SE}, // 00 In Hangar
	{   53,   27, {},                                                                     Direction::N }, // 01 Taxi to right outside depot
	{   32,   23, {AirportMovingDataFlag::ExactPosition},                                 Direction::NW}, // 02 Terminal 1
	{   10,   23, {AirportMovingDataFlag::ExactPosition},                                 Direction::NW}, // 03 Terminal 2
	{   43,   37, {},                                                                     Direction::N }, // 04 Going towards terminal 2
	{   24,   37, {},                                                                     Direction::N }, // 05 Going towards terminal 2
	{   53,   37, {},                                                                     Direction::N }, // 06 Going for takeoff
	{   61,   40, {AirportMovingDataFlag::ExactPosition},                                 Direction::NE}, // 07 Taxi to start of runway (takeoff)
	{    3,   40, {AirportMovingDataFlag::NoSpeedClamp},                                  Direction::N }, // 08 Accelerate to end of runway
	{  -79,   40, {AirportMovingDataFlag::NoSpeedClamp, AirportMovingDataFlag::Takeoff},  Direction::N }, // 09 Take off
	{  177,   40, {AirportMovingDataFlag::NoSpeedClamp, AirportMovingDataFlag::SlowTurn}, Direction::N }, // 10 Fly to landing position in air
	{   56,   40, {AirportMovingDataFlag::NoSpeedClamp, AirportMovingDataFlag::Land},     Direction::N }, // 11 Going down for land
	{    3,   40, {AirportMovingDataFlag::NoSpeedClamp, AirportMovingDataFlag::Brake},    Direction::N }, // 12 Just landed, brake until end of runway
	{    7,   40, {},                                                                     Direction::N }, // 13 Just landed, turn around and taxi 1 square
	{   53,   40, {},                                                                     Direction::N }, // 14 Taxi from runway to crossing
	{    1,  193, {AirportMovingDataFlag::NoSpeedClamp, AirportMovingDataFlag::SlowTurn}, Direction::N }, // 15 Fly around waiting for a landing spot (north-east)
	{    1,    1, {AirportMovingDataFlag::NoSpeedClamp, AirportMovingDataFlag::SlowTurn}, Direction::N }, // 16 Fly around waiting for a landing spot (north-west)
	{  257,    1, {AirportMovingDataFlag::NoSpeedClamp, AirportMovingDataFlag::SlowTurn}, Direction::N }, // 17 Fly around waiting for a landing spot (south-west)
	{  273,   47, {AirportMovingDataFlag::NoSpeedClamp, AirportMovingDataFlag::SlowTurn}, Direction::N }, // 18 Fly around waiting for a landing spot (south)
	{   44,   37, {AirportMovingDataFlag::HeliRaise},                                     Direction::N }, // 19 Helicopter takeoff
	{   44,   40, {AirportMovingDataFlag::NoSpeedClamp, AirportMovingDataFlag::SlowTurn}, Direction::N }, // 20 In position above landing spot helicopter
	{   44,   40, {AirportMovingDataFlag::HeliLower},                                     Direction::N }, // 21 Helicopter landing
};

/** Commuter Airfield (small) 5x4. */
static const AirportMovingData _airport_moving_data_commuter[38] = {
	{   69,    3, {AirportMovingDataFlag::ExactPosition},                                 Direction::SE}, // 00 In Hangar
	{   72,   22, {},                                                                     Direction::N }, // 01 Taxi to right outside depot
	{    8,   22, {AirportMovingDataFlag::ExactPosition},                                 Direction::SW}, // 02 Taxi to right outside depot
	{   24,   36, {AirportMovingDataFlag::ExactPosition},                                 Direction::SE}, // 03 Terminal 1
	{   40,   36, {AirportMovingDataFlag::ExactPosition},                                 Direction::SE}, // 04 Terminal 2
	{   56,   36, {AirportMovingDataFlag::ExactPosition},                                 Direction::SE}, // 05 Terminal 3
	{   40,    8, {AirportMovingDataFlag::ExactPosition},                                 Direction::NE}, // 06 Helipad 1
	{   56,    8, {AirportMovingDataFlag::ExactPosition},                                 Direction::NE}, // 07 Helipad 2
	{   24,   22, {},                                                                     Direction::SW}, // 08 Taxiing
	{   40,   22, {},                                                                     Direction::SW}, // 09 Taxiing
	{   56,   22, {},                                                                     Direction::SW}, // 10 Taxiing
	{   72,   40, {},                                                                     Direction::SE}, // 11 Airport OUTWAY
	{   72,   54, {AirportMovingDataFlag::ExactPosition},                                 Direction::NE}, // 12 Accelerate to end of runway
	{    7,   54, {AirportMovingDataFlag::NoSpeedClamp},                                  Direction::N }, // 13 Release control of runway, for smoother movement
	{    5,   54, {AirportMovingDataFlag::NoSpeedClamp},                                  Direction::N }, // 14 End of runway
	{  -79,   54, {AirportMovingDataFlag::NoSpeedClamp, AirportMovingDataFlag::Takeoff},  Direction::N }, // 15 Take off
	{  145,   54, {AirportMovingDataFlag::NoSpeedClamp, AirportMovingDataFlag::SlowTurn}, Direction::N }, // 16 Fly to landing position in air
	{   73,   54, {AirportMovingDataFlag::NoSpeedClamp, AirportMovingDataFlag::Land},     Direction::N }, // 17 Going down for land
	{    3,   54, {AirportMovingDataFlag::NoSpeedClamp, AirportMovingDataFlag::Brake},    Direction::N }, // 18 Just landed, brake until end of runway
	{   12,   54, {AirportMovingDataFlag::SlowTurn},                                      Direction::NW}, // 19 Just landed, turn around and taxi
	{    8,   32, {},                                                                     Direction::NW}, // 20 Taxi from runway to crossing
	{    1,  149, {AirportMovingDataFlag::NoSpeedClamp, AirportMovingDataFlag::SlowTurn}, Direction::N }, // 21 Fly around waiting for a landing spot (north-east)
	{    1,    6, {AirportMovingDataFlag::NoSpeedClamp, AirportMovingDataFlag::SlowTurn}, Direction::N }, // 22 Fly around waiting for a landing spot (north-west)
	{  193,    6, {AirportMovingDataFlag::NoSpeedClamp, AirportMovingDataFlag::SlowTurn}, Direction::N }, // 23 Fly around waiting for a landing spot (south-west)
	{  225,   62, {AirportMovingDataFlag::NoSpeedClamp, AirportMovingDataFlag::SlowTurn}, Direction::N }, // 24 Fly around waiting for a landing spot (south)
	/* Helicopter */
	{   80,    0, {AirportMovingDataFlag::NoSpeedClamp, AirportMovingDataFlag::SlowTurn}, Direction::N }, // 25 Bufferspace before helipad
	{   80,    0, {AirportMovingDataFlag::NoSpeedClamp, AirportMovingDataFlag::SlowTurn}, Direction::N }, // 26 Bufferspace before helipad
	{   32,    8, {AirportMovingDataFlag::NoSpeedClamp, AirportMovingDataFlag::SlowTurn}, Direction::N }, // 27 Get in position for Helipad1
	{   48,    8, {AirportMovingDataFlag::NoSpeedClamp, AirportMovingDataFlag::SlowTurn}, Direction::N }, // 28 Get in position for Helipad2
	{   32,    8, {AirportMovingDataFlag::HeliLower},                                     Direction::N }, // 29 Land at Helipad1
	{   48,    8, {AirportMovingDataFlag::HeliLower},                                     Direction::N }, // 30 Land at Helipad2
	{   32,    8, {AirportMovingDataFlag::HeliRaise},                                     Direction::N }, // 31 Takeoff Helipad1
	{   48,    8, {AirportMovingDataFlag::HeliRaise},                                     Direction::N }, // 32 Takeoff Helipad2
	{   64,   22, {AirportMovingDataFlag::NoSpeedClamp, AirportMovingDataFlag::SlowTurn}, Direction::N }, // 33 Go to position for Hangarentrance in air
	{   64,   22, {AirportMovingDataFlag::HeliLower},                                     Direction::N }, // 34 Land in front of hangar
	{   40,    8, {AirportMovingDataFlag::ExactPosition},                                 Direction::N }, // 35 pre-helitakeoff helipad 1
	{   56,    8, {AirportMovingDataFlag::ExactPosition},                                 Direction::N }, // 36 pre-helitakeoff helipad 2
	{   64,   25, {AirportMovingDataFlag::HeliRaise},                                     Direction::N }, // 37 Take off in front of hangar
};

/** City Airport (large) 6x6. */
static const AirportMovingData _airport_moving_data_city[] = {
	{   85,    3, {AirportMovingDataFlag::ExactPosition},                                 Direction::SE}, // 00 In Hangar
	{   85,   22, {},                                                                     Direction::N }, // 01 Taxi to right outside depot
	{   26,   41, {AirportMovingDataFlag::ExactPosition},                                 Direction::SW}, // 02 Terminal 1
	{   56,   22, {AirportMovingDataFlag::ExactPosition},                                 Direction::SE}, // 03 Terminal 2
	{   38,    8, {AirportMovingDataFlag::ExactPosition},                                 Direction::SW}, // 04 Terminal 3
	{   65,    6, {},                                                                     Direction::N }, // 05 Taxi to right in infront of terminal 2/3
	{   80,   27, {},                                                                     Direction::N }, // 06 Taxiway terminals 2-3
	{   44,   63, {},                                                                     Direction::N }, // 07 Taxi to Airport center
	{   58,   71, {},                                                                     Direction::N }, // 08 Towards takeoff
	{   72,   85, {},                                                                     Direction::N }, // 09 Taxi to runway (takeoff)
	{   89,   85, {AirportMovingDataFlag::ExactPosition},                                 Direction::NE}, // 10 Taxi to start of runway (takeoff)
	{    3,   85, {AirportMovingDataFlag::NoSpeedClamp},                                  Direction::N }, // 11 Accelerate to end of runway
	{  -79,   85, {AirportMovingDataFlag::NoSpeedClamp, AirportMovingDataFlag::Takeoff},  Direction::N }, // 12 Take off
	{  177,   87, {AirportMovingDataFlag::Hold,         AirportMovingDataFlag::SlowTurn}, Direction::N }, // 13 Fly to landing position in air
	{   89,   87, {AirportMovingDataFlag::Hold,         AirportMovingDataFlag::Land},     Direction::N }, // 14 Going down for land
	{   20,   87, {AirportMovingDataFlag::NoSpeedClamp, AirportMovingDataFlag::Brake},    Direction::N }, // 15 Just landed, brake until end of runway
	{   20,   87, {},                                                                     Direction::N }, // 16 Just landed, turn around and taxi 1 square // NOT USED
	{   36,   71, {},                                                                     Direction::N }, // 17 Taxi from runway to crossing
	{  160,   87, {AirportMovingDataFlag::Hold,         AirportMovingDataFlag::SlowTurn}, Direction::N }, // 18 Fly around waiting for a landing spot (north-east)
	{  140,    1, {AirportMovingDataFlag::NoSpeedClamp, AirportMovingDataFlag::SlowTurn}, Direction::N }, // 19 Final approach fix
	{  257,    1, {AirportMovingDataFlag::Hold,         AirportMovingDataFlag::SlowTurn}, Direction::N }, // 20 Fly around waiting for a landing spot (south-west)
	{  273,   49, {AirportMovingDataFlag::Hold,         AirportMovingDataFlag::SlowTurn}, Direction::N }, // 21 Fly around waiting for a landing spot (south)
	{   44,   63, {AirportMovingDataFlag::HeliRaise},                                     Direction::N }, // 22 Helicopter takeoff
	{   28,   74, {AirportMovingDataFlag::NoSpeedClamp, AirportMovingDataFlag::SlowTurn}, Direction::N }, // 23 In position above landing spot helicopter
	{   28,   74, {AirportMovingDataFlag::HeliLower},                                     Direction::N }, // 24 Helicopter landing
	{  145,    1, {AirportMovingDataFlag::Hold,         AirportMovingDataFlag::SlowTurn}, Direction::N }, // 25 Fly around waiting for a landing spot (north-west)
	{  -32,    1, {AirportMovingDataFlag::NoSpeedClamp, AirportMovingDataFlag::SlowTurn}, Direction::N }, // 26 Initial approach fix (north)
	{  300,  -48, {AirportMovingDataFlag::NoSpeedClamp, AirportMovingDataFlag::SlowTurn}, Direction::N }, // 27 Initial approach fix (south)
	{  140,  -48, {AirportMovingDataFlag::NoSpeedClamp, AirportMovingDataFlag::SlowTurn}, Direction::N }, // 28 Intermediate Approach fix (south), IAF (west)
	{  -32,  120, {AirportMovingDataFlag::NoSpeedClamp, AirportMovingDataFlag::SlowTurn}, Direction::N }, // 29 Initial approach fix (east)
};

/** Metropolitan Airport (metropolitan) - 2 runways. */
static const AirportMovingData _airport_moving_data_metropolitan[28] = {
	{   85,    3, {AirportMovingDataFlag::ExactPosition},                                 Direction::SE}, // 00 In Hangar
	{   85,   22, {},                                                                     Direction::N }, // 01 Taxi to right outside depot
	{   26,   41, {AirportMovingDataFlag::ExactPosition},                                 Direction::SW}, // 02 Terminal 1
	{   56,   22, {AirportMovingDataFlag::ExactPosition},                                 Direction::SE}, // 03 Terminal 2
	{   38,    8, {AirportMovingDataFlag::ExactPosition},                                 Direction::SW}, // 04 Terminal 3
	{   65,    6, {},                                                                     Direction::N }, // 05 Taxi to right in infront of terminal 2/3
	{   80,   27, {},                                                                     Direction::N }, // 06 Taxiway terminals 2-3
	{   49,   58, {},                                                                     Direction::N }, // 07 Taxi to Airport center
	{   72,   58, {},                                                                     Direction::N }, // 08 Towards takeoff
	{   72,   69, {},                                                                     Direction::N }, // 09 Taxi to runway (takeoff)
	{   89,   69, {AirportMovingDataFlag::ExactPosition},                                 Direction::NE}, // 10 Taxi to start of runway (takeoff)
	{    3,   69, {AirportMovingDataFlag::NoSpeedClamp},                                  Direction::N }, // 11 Accelerate to end of runway
	{  -79,   69, {AirportMovingDataFlag::NoSpeedClamp, AirportMovingDataFlag::Takeoff},  Direction::N }, // 12 Take off
	{  177,   85, {AirportMovingDataFlag::NoSpeedClamp, AirportMovingDataFlag::SlowTurn}, Direction::N }, // 13 Fly to landing position in air
	{   89,   85, {AirportMovingDataFlag::NoSpeedClamp, AirportMovingDataFlag::Land},     Direction::N }, // 14 Going down for land
	{    3,   85, {AirportMovingDataFlag::NoSpeedClamp, AirportMovingDataFlag::Brake},    Direction::N }, // 15 Just landed, brake until end of runway
	{   21,   85, {},                                                                     Direction::N }, // 16 Just landed, turn around and taxi 1 square
	{   21,   69, {},                                                                     Direction::N }, // 17 On Runway-out taxiing to In-Way
	{   21,   58, {AirportMovingDataFlag::ExactPosition},                                 Direction::SW}, // 18 Taxi from runway to crossing
	{    1,  193, {AirportMovingDataFlag::NoSpeedClamp, AirportMovingDataFlag::SlowTurn}, Direction::N }, // 19 Fly around waiting for a landing spot (north-east)
	{    1,    1, {AirportMovingDataFlag::NoSpeedClamp, AirportMovingDataFlag::SlowTurn}, Direction::N }, // 20 Fly around waiting for a landing spot (north-west)
	{  257,    1, {AirportMovingDataFlag::NoSpeedClamp, AirportMovingDataFlag::SlowTurn}, Direction::N }, // 21 Fly around waiting for a landing spot (south-west)
	{  273,   49, {AirportMovingDataFlag::NoSpeedClamp, AirportMovingDataFlag::SlowTurn}, Direction::N }, // 22 Fly around waiting for a landing spot (south)
	{   44,   58, {},                                                                     Direction::N }, // 23 Helicopter takeoff spot on ground (to clear airport sooner)
	{   44,   63, {AirportMovingDataFlag::HeliRaise},                                     Direction::N }, // 24 Helicopter takeoff
	{   15,   54, {AirportMovingDataFlag::NoSpeedClamp, AirportMovingDataFlag::SlowTurn}, Direction::N }, // 25 Get in position above landing spot helicopter
	{   15,   54, {AirportMovingDataFlag::HeliLower},                                     Direction::N }, // 26 Helicopter landing
	{   21,   58, {AirportMovingDataFlag::ExactPosition},                                 Direction::SW}, // 27 Transitions after landing to on-ground movement
};

/** International Airport (international) - 2 runways, 6 terminals, dedicated helipad. */
static const AirportMovingData _airport_moving_data_international[53] = {
	{    7,   55, {AirportMovingDataFlag::ExactPosition},                                 Direction::SE}, // 00 In Hangar 1
	{  100,   21, {AirportMovingDataFlag::ExactPosition},                                 Direction::SE}, // 01 In Hangar 2
	{    7,   70, {},                                                                     Direction::N }, // 02 Taxi to right outside depot (Hangar 1)
	{  100,   36, {},                                                                     Direction::N }, // 03 Taxi to right outside depot (Hangar 2)
	{   38,   70, {AirportMovingDataFlag::ExactPosition},                                 Direction::SW}, // 04 Terminal 1
	{   38,   54, {AirportMovingDataFlag::ExactPosition},                                 Direction::SW}, // 05 Terminal 2
	{   38,   38, {AirportMovingDataFlag::ExactPosition},                                 Direction::SW}, // 06 Terminal 3
	{   70,   70, {AirportMovingDataFlag::ExactPosition},                                 Direction::NE}, // 07 Terminal 4
	{   70,   54, {AirportMovingDataFlag::ExactPosition},                                 Direction::NE}, // 08 Terminal 5
	{   70,   38, {AirportMovingDataFlag::ExactPosition},                                 Direction::NE}, // 09 Terminal 6
	{  104,   71, {AirportMovingDataFlag::ExactPosition},                                 Direction::NE}, // 10 Helipad 1
	{  104,   55, {AirportMovingDataFlag::ExactPosition},                                 Direction::NE}, // 11 Helipad 2
	{   22,   87, {},                                                                     Direction::N }, // 12 Towards Terminals 4/5/6, Helipad 1/2
	{   60,   87, {},                                                                     Direction::N }, // 13 Towards Terminals 4/5/6, Helipad 1/2
	{   66,   87, {},                                                                     Direction::N }, // 14 Towards Terminals 4/5/6, Helipad 1/2
	{   86,   87, {AirportMovingDataFlag::ExactPosition},                                 Direction::NW}, // 15 Towards Terminals 4/5/6, Helipad 1/2
	{   86,   70, {},                                                                     Direction::N }, // 16 In Front of Terminal 4 / Helipad 1
	{   86,   54, {},                                                                     Direction::N }, // 17 In Front of Terminal 5 / Helipad 2
	{   86,   38, {},                                                                     Direction::N }, // 18 In Front of Terminal 6
	{   86,   22, {},                                                                     Direction::N }, // 19 Towards Terminals Takeoff (Taxiway)
	{   66,   22, {},                                                                     Direction::N }, // 20 Towards Terminals Takeoff (Taxiway)
	{   60,   22, {},                                                                     Direction::N }, // 21 Towards Terminals Takeoff (Taxiway)
	{   38,   22, {},                                                                     Direction::N }, // 22 Towards Terminals Takeoff (Taxiway)
	{   22,   70, {},                                                                     Direction::N }, // 23 In Front of Terminal 1
	{   22,   58, {},                                                                     Direction::N }, // 24 In Front of Terminal 2
	{   22,   38, {},                                                                     Direction::N }, // 25 In Front of Terminal 3
	{   22,   22, {AirportMovingDataFlag::ExactPosition},                                 Direction::NW}, // 26 Going for Takeoff
	{   22,    6, {},                                                                     Direction::N }, // 27 On Runway-out, prepare for takeoff
	{    3,    6, {AirportMovingDataFlag::ExactPosition},                                 Direction::SW}, // 28 Accelerate to end of runway
	{   60,    6, {AirportMovingDataFlag::NoSpeedClamp},                                  Direction::N }, // 29 Release control of runway, for smoother movement
	{  105,    6, {AirportMovingDataFlag::NoSpeedClamp},                                  Direction::N }, // 30 End of runway
	{  190,    6, {AirportMovingDataFlag::NoSpeedClamp, AirportMovingDataFlag::Takeoff},  Direction::N }, // 31 Take off
	{  193,  104, {AirportMovingDataFlag::NoSpeedClamp, AirportMovingDataFlag::SlowTurn}, Direction::N }, // 32 Fly to landing position in air
	{  105,  104, {AirportMovingDataFlag::NoSpeedClamp, AirportMovingDataFlag::Land},     Direction::N }, // 33 Going down for land
	{    3,  104, {AirportMovingDataFlag::NoSpeedClamp, AirportMovingDataFlag::Brake},    Direction::N }, // 34 Just landed, brake until end of runway
	{   12,  104, {AirportMovingDataFlag::SlowTurn},                                      Direction::N }, // 35 Just landed, turn around and taxi 1 square
	{    7,   84, {},                                                                     Direction::N }, // 36 Taxi from runway to crossing
	{    1,  209, {AirportMovingDataFlag::NoSpeedClamp, AirportMovingDataFlag::SlowTurn}, Direction::N }, // 37 Fly around waiting for a landing spot (north-east)
	{    1,    6, {AirportMovingDataFlag::NoSpeedClamp, AirportMovingDataFlag::SlowTurn}, Direction::N }, // 38 Fly around waiting for a landing spot (north-west)
	{  273,    6, {AirportMovingDataFlag::NoSpeedClamp, AirportMovingDataFlag::SlowTurn}, Direction::N }, // 39 Fly around waiting for a landing spot (south-west)
	{  305,   81, {AirportMovingDataFlag::NoSpeedClamp, AirportMovingDataFlag::SlowTurn}, Direction::N }, // 40 Fly around waiting for a landing spot (south)
	/* Helicopter */
	{  128,   80, {AirportMovingDataFlag::NoSpeedClamp, AirportMovingDataFlag::SlowTurn}, Direction::N }, // 41 Bufferspace before helipad
	{  128,   80, {AirportMovingDataFlag::NoSpeedClamp, AirportMovingDataFlag::SlowTurn}, Direction::N }, // 42 Bufferspace before helipad
	{   96,   71, {AirportMovingDataFlag::NoSpeedClamp, AirportMovingDataFlag::SlowTurn}, Direction::N }, // 43 Get in position for Helipad1
	{   96,   55, {AirportMovingDataFlag::NoSpeedClamp, AirportMovingDataFlag::SlowTurn}, Direction::N }, // 44 Get in position for Helipad2
	{   96,   71, {AirportMovingDataFlag::HeliLower},                                     Direction::N }, // 45 Land at Helipad1
	{   96,   55, {AirportMovingDataFlag::HeliLower},                                     Direction::N }, // 46 Land at Helipad2
	{  104,   71, {AirportMovingDataFlag::HeliRaise},                                     Direction::N }, // 47 Takeoff Helipad1
	{  104,   55, {AirportMovingDataFlag::HeliRaise},                                     Direction::N }, // 48 Takeoff Helipad2
	{  104,   32, {AirportMovingDataFlag::NoSpeedClamp, AirportMovingDataFlag::SlowTurn}, Direction::N }, // 49 Go to position for Hangarentrance in air
	{  104,   32, {AirportMovingDataFlag::HeliLower},                                     Direction::N }, // 50 Land in HANGAR2_AREA to go to hangar
	{    7,   70, {AirportMovingDataFlag::HeliRaise},                                     Direction::N }, // 51 Takeoff from HANGAR1_AREA
	{  100,   36, {AirportMovingDataFlag::HeliRaise},                                     Direction::N }, // 52 Takeoff from HANGAR2_AREA
};

/** Intercontinental Airport - 4 runways, 8 terminals, 2 dedicated helipads. */
static const AirportMovingData _airport_moving_data_intercontinental[77] = {
	{    8,   87, {AirportMovingDataFlag::ExactPosition},                                 Direction::SE}, // 00 In Hangar 1
	{  136,   72, {AirportMovingDataFlag::ExactPosition},                                 Direction::SE}, // 01 In Hangar 2
	{    8,  104, {},                                                                     Direction::N }, // 02 Taxi to right outside depot 1
	{  136,   88, {},                                                                     Direction::N }, // 03 Taxi to right outside depot 2
	{   56,  120, {AirportMovingDataFlag::ExactPosition},                                 Direction::W }, // 04 Terminal 1
	{   56,  104, {AirportMovingDataFlag::ExactPosition},                                 Direction::SW}, // 05 Terminal 2
	{   56,   88, {AirportMovingDataFlag::ExactPosition},                                 Direction::SW}, // 06 Terminal 3
	{   56,   72, {AirportMovingDataFlag::ExactPosition},                                 Direction::SW}, // 07 Terminal 4
	{   88,  120, {AirportMovingDataFlag::ExactPosition},                                 Direction::N }, // 08 Terminal 5
	{   88,  104, {AirportMovingDataFlag::ExactPosition},                                 Direction::NE}, // 09 Terminal 6
	{   88,   88, {AirportMovingDataFlag::ExactPosition},                                 Direction::NE}, // 10 Terminal 7
	{   88,   72, {AirportMovingDataFlag::ExactPosition},                                 Direction::NE}, // 11 Terminal 8
	{   88,   56, {AirportMovingDataFlag::ExactPosition},                                 Direction::SE}, // 12 Helipad 1
	{   72,   56, {AirportMovingDataFlag::ExactPosition},                                 Direction::NE}, // 13 Helipad 2
	{   40,  136, {},                                                                     Direction::N }, // 14 Term group 2 enter 1 a
	{   56,  136, {},                                                                     Direction::N }, // 15 Term group 2 enter 1 b
	{   88,  136, {},                                                                     Direction::N }, // 16 Term group 2 enter 2 a
	{  104,  136, {},                                                                     Direction::N }, // 17 Term group 2 enter 2 b
	{  104,  120, {},                                                                     Direction::N }, // 18 Term group 2 - opp term 5
	{  104,  104, {},                                                                     Direction::N }, // 19 Term group 2 - opp term 6 & exit2
	{  104,   88, {},                                                                     Direction::N }, // 20 Term group 2 - opp term 7 & hangar area 2
	{  104,   72, {},                                                                     Direction::N }, // 21 Term group 2 - opp term 8
	{  104,   56, {},                                                                     Direction::N }, // 22 Taxi Term group 2 exit a
	{  104,   40, {},                                                                     Direction::N }, // 23 Taxi Term group 2 exit b
	{   56,   40, {},                                                                     Direction::N }, // 24 Term group 2 exit 2a
	{   40,   40, {},                                                                     Direction::N }, // 25 Term group 2 exit 2b
	{   40,  120, {},                                                                     Direction::N }, // 26 Term group 1 - opp term 1
	{   40,  104, {},                                                                     Direction::N }, // 27 Term group 1 - opp term 2 & hangar area 1
	{   40,   88, {},                                                                     Direction::N }, // 28 Term group 1 - opp term 3
	{   40,   72, {},                                                                     Direction::N }, // 29 Term group 1 - opp term 4
	{   18,   72, {},                                                                     Direction::NW}, // 30 Outway 1
	{    8,   40, {},                                                                     Direction::NW}, // 31 Airport OUTWAY
	{    8,   24, {AirportMovingDataFlag::ExactPosition},                                 Direction::SW}, // 32 Accelerate to end of runway
	{  119,   24, {AirportMovingDataFlag::NoSpeedClamp},                                  Direction::N }, // 33 Release control of runway, for smoother movement
	{  117,   24, {AirportMovingDataFlag::NoSpeedClamp},                                  Direction::N }, // 34 End of runway
	{  197,   24, {AirportMovingDataFlag::NoSpeedClamp, AirportMovingDataFlag::Takeoff},  Direction::N }, // 35 Take off
	{  254,   84, {AirportMovingDataFlag::NoSpeedClamp, AirportMovingDataFlag::SlowTurn}, Direction::N }, // 36 Flying to landing position in air
	{  117,  168, {AirportMovingDataFlag::NoSpeedClamp, AirportMovingDataFlag::Land},     Direction::N }, // 37 Going down for land
	{    8,  168, {AirportMovingDataFlag::NoSpeedClamp, AirportMovingDataFlag::Brake},    Direction::N }, // 38 Just landed, brake until end of runway
	{    8,  168, {},                                                                     Direction::N }, // 39 Just landed, turn around and taxi
	{    8,  144, {},                                                                     Direction::NW}, // 40 Taxi from runway
	{    8,  128, {},                                                                     Direction::NW}, // 41 Taxi from runway
	{    8,  120, {AirportMovingDataFlag::ExactPosition},                                 Direction::NW}, // 42 Airport entrance
	{   56,  344, {AirportMovingDataFlag::NoSpeedClamp, AirportMovingDataFlag::SlowTurn}, Direction::N }, // 43 Fly around waiting for a landing spot (north-east)
	{ -200,   88, {AirportMovingDataFlag::NoSpeedClamp, AirportMovingDataFlag::SlowTurn}, Direction::N }, // 44 Fly around waiting for a landing spot (north-west)
	{   56, -168, {AirportMovingDataFlag::NoSpeedClamp, AirportMovingDataFlag::SlowTurn}, Direction::N }, // 45 Fly around waiting for a landing spot (south-west)
	{  312,   88, {AirportMovingDataFlag::NoSpeedClamp, AirportMovingDataFlag::SlowTurn}, Direction::N }, // 46 Fly around waiting for a landing spot (south)
	/* Helicopter */
	{   96,   40, {AirportMovingDataFlag::NoSpeedClamp},                                  Direction::N }, // 47 Bufferspace before helipad
	{   96,   40, {AirportMovingDataFlag::NoSpeedClamp},                                  Direction::N }, // 48 Bufferspace before helipad
	{   82,   54, {AirportMovingDataFlag::NoSpeedClamp, AirportMovingDataFlag::SlowTurn}, Direction::N }, // 49 Get in position for Helipad1
	{   64,   56, {AirportMovingDataFlag::NoSpeedClamp, AirportMovingDataFlag::SlowTurn}, Direction::N }, // 50 Get in position for Helipad2
	{   81,   55, {AirportMovingDataFlag::HeliLower},                                     Direction::N }, // 51 Land at Helipad1
	{   64,   56, {AirportMovingDataFlag::HeliLower},                                     Direction::N }, // 52 Land at Helipad2
	{   80,   56, {AirportMovingDataFlag::HeliRaise},                                     Direction::N }, // 53 Takeoff Helipad1
	{   64,   56, {AirportMovingDataFlag::HeliRaise},                                     Direction::N }, // 54 Takeoff Helipad2
	{  136,   96, {AirportMovingDataFlag::NoSpeedClamp, AirportMovingDataFlag::SlowTurn}, Direction::N }, // 55 Go to position for Hangarentrance in air
	{  136,   96, {AirportMovingDataFlag::HeliLower},                                     Direction::N }, // 56 Land in front of hangar2
	{  126,  104, {},                                                                     Direction::SE}, // 57 Outway 2
	{  136,  136, {},                                                                     Direction::NE}, // 58 Airport OUTWAY 2
	{  136,  152, {AirportMovingDataFlag::ExactPosition},                                 Direction::NE}, // 59 Accelerate to end of runway2
	{   16,  152, {AirportMovingDataFlag::NoSpeedClamp},                                  Direction::N }, // 60 Release control of runway2, for smoother movement
	{   20,  152, {AirportMovingDataFlag::NoSpeedClamp},                                  Direction::N }, // 61 End of runway2
	{  -56,  152, {AirportMovingDataFlag::NoSpeedClamp, AirportMovingDataFlag::Takeoff},  Direction::N }, // 62 Take off2
	{   24,    8, {AirportMovingDataFlag::NoSpeedClamp, AirportMovingDataFlag::Land},     Direction::N }, // 63 Going down for land2
	{  136,    8, {AirportMovingDataFlag::NoSpeedClamp, AirportMovingDataFlag::Brake},    Direction::N }, // 64 Just landed, brake until end of runway2in
	{  136,    8, {},                                                                     Direction::N }, // 65 Just landed, turn around and taxi
	{  136,   24, {},                                                                     Direction::SE}, // 66 Taxi from runway 2in
	{  136,   40, {},                                                                     Direction::SE}, // 67 Taxi from runway 2in
	{  136,   56, {AirportMovingDataFlag::ExactPosition},                                 Direction::NE}, // 68 Airport entrance2
	{  -56,    8, {AirportMovingDataFlag::NoSpeedClamp, AirportMovingDataFlag::SlowTurn}, Direction::N }, // 69 Fly to landing position in air2
	{   88,   40, {},                                                                     Direction::N }, // 70 Taxi Term group 2 exit - opp heli1
	{   72,   40, {},                                                                     Direction::N }, // 71 Taxi Term group 2 exit - opp heli2
	{   88,   57, {AirportMovingDataFlag::ExactPosition},                                 Direction::SE}, // 72 pre-helitakeoff helipad 1
	{   71,   56, {AirportMovingDataFlag::ExactPosition},                                 Direction::NE}, // 73 pre-helitakeoff helipad 2
	{    8,  120, {AirportMovingDataFlag::HeliRaise},                                     Direction::N }, // 74 Helitakeoff outside depot 1
	{  136,  104, {AirportMovingDataFlag::HeliRaise},                                     Direction::N }, // 75 Helitakeoff outside depot 2
	{  197,  168, {AirportMovingDataFlag::NoSpeedClamp, AirportMovingDataFlag::SlowTurn}, Direction::N }, // 76 Fly to landing position in air1
};


/** Heliport (heliport). */
static const AirportMovingData _airport_moving_data_heliport[9] = {
	{    5,    9, {AirportMovingDataFlag::ExactPosition},                                 Direction::NE}, // 0 - At heliport terminal
	{    2,    9, {AirportMovingDataFlag::HeliRaise},                                     Direction::N }, // 1 - Take off (play sound)
	{   -3,    9, {AirportMovingDataFlag::NoSpeedClamp, AirportMovingDataFlag::SlowTurn}, Direction::N }, // 2 - In position above landing spot helicopter
	{   -3,    9, {AirportMovingDataFlag::HeliLower},                                     Direction::N }, // 3 - Land
	{    2,    9, {},                                                                     Direction::N }, // 4 - Goto terminal on ground
	{  -31,   59, {AirportMovingDataFlag::NoSpeedClamp, AirportMovingDataFlag::SlowTurn}, Direction::N }, // 5 - Circle #1 (north-east)
	{  -31,  -49, {AirportMovingDataFlag::NoSpeedClamp, AirportMovingDataFlag::SlowTurn}, Direction::N }, // 6 - Circle #2 (north-west)
	{   49,  -49, {AirportMovingDataFlag::NoSpeedClamp, AirportMovingDataFlag::SlowTurn}, Direction::N }, // 7 - Circle #3 (south-west)
	{   70,    9, {AirportMovingDataFlag::NoSpeedClamp, AirportMovingDataFlag::SlowTurn}, Direction::N }, // 8 - Circle #4 (south)
};

/** HeliDepot 2x2 (heliport). */
static const AirportMovingData _airport_moving_data_helidepot[18] = {
	{   24,    4, {AirportMovingDataFlag::ExactPosition},                                  Direction::NE}, // 0 - At depot
	{   24,   28, {},                                                                      Direction::N }, // 1 Taxi to right outside depot
	{    5,   38, {AirportMovingDataFlag::NoSpeedClamp, AirportMovingDataFlag::SlowTurn},  Direction::N }, // 2 Flying
	{  -15,  -15, {AirportMovingDataFlag::NoSpeedClamp, AirportMovingDataFlag::SlowTurn},  Direction::N }, // 3 - Circle #1 (north-east)
	{  -15,  -49, {AirportMovingDataFlag::NoSpeedClamp, AirportMovingDataFlag::SlowTurn},  Direction::N }, // 4 - Circle #2 (north-west)
	{   49,  -49, {AirportMovingDataFlag::NoSpeedClamp, AirportMovingDataFlag::SlowTurn},  Direction::N }, // 5 - Circle #3 (south-west)
	{   49,  -15, {AirportMovingDataFlag::NoSpeedClamp, AirportMovingDataFlag::SlowTurn},  Direction::N }, // 6 - Circle #4 (south-east)
	{    8,   32, {AirportMovingDataFlag::NoSpeedClamp, AirportMovingDataFlag::SlowTurn},  Direction::NW}, // 7 - PreHelipad
	{    8,   32, {AirportMovingDataFlag::NoSpeedClamp, AirportMovingDataFlag::SlowTurn},  Direction::NW}, // 8 - Helipad
	{    8,   16, {AirportMovingDataFlag::NoSpeedClamp, AirportMovingDataFlag::SlowTurn},  Direction::NW}, // 9 - Land
	{    8,   16, {AirportMovingDataFlag::HeliLower},                                      Direction::NW}, // 10 - Land
	{    8,   24, {AirportMovingDataFlag::HeliRaise},                                      Direction::N }, // 11 - Take off (play sound)
	{   32,   24, {AirportMovingDataFlag::NoSpeedClamp, AirportMovingDataFlag::SlowTurn},  Direction::NW}, // 12 Air to above hangar area
	{   32,   24, {AirportMovingDataFlag::HeliLower},                                      Direction::NW}, // 13 Taxi to right outside depot
	{    8,   24, {AirportMovingDataFlag::ExactPosition},                                  Direction::NW}, // 14 - on helipad1
	{   24,   28, {AirportMovingDataFlag::HeliRaise},                                      Direction::N }, // 15 Takeoff right outside depot
	{    8,   24, {AirportMovingDataFlag::HeliRaise},                                      Direction::SW}, // 16 - Take off (play sound)
	{    8,   24, {AirportMovingDataFlag::SlowTurn, AirportMovingDataFlag::ExactPosition}, Direction::E }, // 17 - turn on helipad1 for takeoff
};

/** HeliDepot 2x2 (heliport). */
static const AirportMovingData _airport_moving_data_helistation[33] = {
	{    8,    3, {AirportMovingDataFlag::ExactPosition},                                 Direction::SE}, // 00 In Hangar2
	{    8,   22, {},                                                                     Direction::N }, // 01 outside hangar 2
	{  116,   24, {AirportMovingDataFlag::NoSpeedClamp, AirportMovingDataFlag::SlowTurn}, Direction::N }, // 02 Fly to landing position in air
	{   14,   22, {AirportMovingDataFlag::HeliRaise},                                     Direction::N }, // 03 Helitakeoff outside hangar1(play sound)
	{   24,   22, {},                                                                     Direction::N }, // 04 taxiing
	{   40,   22, {},                                                                     Direction::N }, // 05 taxiing
	{   40,    8, {AirportMovingDataFlag::ExactPosition},                                 Direction::NE}, // 06 Helipad 1
	{   56,    8, {AirportMovingDataFlag::ExactPosition},                                 Direction::NE}, // 07 Helipad 2
	{   56,   24, {AirportMovingDataFlag::ExactPosition},                                 Direction::NE}, // 08 Helipad 3
	{   40,    8, {AirportMovingDataFlag::ExactPosition},                                 Direction::N }, // 09 pre-helitakeoff helipad 1
	{   56,    8, {AirportMovingDataFlag::ExactPosition},                                 Direction::N }, // 10 pre-helitakeoff helipad 2
	{   56,   24, {AirportMovingDataFlag::ExactPosition},                                 Direction::N }, // 11 pre-helitakeoff helipad 3
	{   32,    8, {AirportMovingDataFlag::HeliRaise},                                     Direction::N }, // 12 Takeoff Helipad1
	{   48,    8, {AirportMovingDataFlag::HeliRaise},                                     Direction::N }, // 13 Takeoff Helipad2
	{   48,   24, {AirportMovingDataFlag::HeliRaise},                                     Direction::N }, // 14 Takeoff Helipad3
	{   84,   24, {AirportMovingDataFlag::NoSpeedClamp, AirportMovingDataFlag::SlowTurn}, Direction::N }, // 15 Bufferspace before helipad
	{   68,   24, {AirportMovingDataFlag::NoSpeedClamp, AirportMovingDataFlag::SlowTurn}, Direction::N }, // 16 Bufferspace before helipad
	{   32,    8, {AirportMovingDataFlag::NoSpeedClamp, AirportMovingDataFlag::SlowTurn}, Direction::N }, // 17 Get in position for Helipad1
	{   48,    8, {AirportMovingDataFlag::NoSpeedClamp, AirportMovingDataFlag::SlowTurn}, Direction::N }, // 18 Get in position for Helipad2
	{   48,   24, {AirportMovingDataFlag::NoSpeedClamp, AirportMovingDataFlag::SlowTurn}, Direction::NE}, // 19 Get in position for Helipad3
	{   40,    8, {AirportMovingDataFlag::HeliLower},                                     Direction::N }, // 20 Land at Helipad1
	{   48,    8, {AirportMovingDataFlag::HeliLower},                                     Direction::N }, // 21 Land at Helipad2
	{   48,   24, {AirportMovingDataFlag::HeliLower},                                     Direction::N }, // 22 Land at Helipad3
	{    0,   22, {AirportMovingDataFlag::NoSpeedClamp, AirportMovingDataFlag::SlowTurn}, Direction::N }, // 23 Go to position for Hangarentrance in air
	{    0,   22, {AirportMovingDataFlag::HeliLower},                                     Direction::N }, // 24 Land in front of hangar
	{  148,   -8, {AirportMovingDataFlag::NoSpeedClamp, AirportMovingDataFlag::SlowTurn}, Direction::N }, // 25 Fly around waiting for a landing spot (south-east)
	{  148,    8, {AirportMovingDataFlag::NoSpeedClamp, AirportMovingDataFlag::SlowTurn}, Direction::N }, // 26 Fly around waiting for a landing spot (south-west)
	{  132,   24, {AirportMovingDataFlag::NoSpeedClamp, AirportMovingDataFlag::SlowTurn}, Direction::N }, // 27 Fly around waiting for a landing spot (south-west)
	{  100,   24, {AirportMovingDataFlag::NoSpeedClamp, AirportMovingDataFlag::SlowTurn}, Direction::N }, // 28 Fly around waiting for a landing spot (north-east)
	{   84,    8, {AirportMovingDataFlag::NoSpeedClamp, AirportMovingDataFlag::SlowTurn}, Direction::N }, // 29 Fly around waiting for a landing spot (south-east)
	{   84,   -8, {AirportMovingDataFlag::NoSpeedClamp, AirportMovingDataFlag::SlowTurn}, Direction::N }, // 30 Fly around waiting for a landing spot (south-west)
	{  100,  -24, {AirportMovingDataFlag::NoSpeedClamp, AirportMovingDataFlag::SlowTurn}, Direction::N }, // 31 Fly around waiting for a landing spot (north-west)
	{  132,  -24, {AirportMovingDataFlag::NoSpeedClamp, AirportMovingDataFlag::SlowTurn}, Direction::N }, // 32 Fly around waiting for a landing spot (north-east)
};

/** Oilrig. */
static const AirportMovingData _airport_moving_data_oilrig[9] = {
	{   31,    9, {AirportMovingDataFlag::ExactPosition},                                 Direction::NE}, // 0 - At oilrig terminal
	{   28,    9, {AirportMovingDataFlag::HeliRaise},                                     Direction::N }, // 1 - Take off (play sound)
	{   23,    9, {AirportMovingDataFlag::NoSpeedClamp, AirportMovingDataFlag::SlowTurn}, Direction::N }, // 2 - In position above landing spot helicopter
	{   23,    9, {AirportMovingDataFlag::HeliLower},                                     Direction::N }, // 3 - Land
	{   28,    9, {},                                                                     Direction::N }, // 4 - Goto terminal on ground
	{  -31,   69, {AirportMovingDataFlag::NoSpeedClamp, AirportMovingDataFlag::SlowTurn}, Direction::N }, // 5 - circle #1 (north-east)
	{  -31,  -49, {AirportMovingDataFlag::NoSpeedClamp, AirportMovingDataFlag::SlowTurn}, Direction::N }, // 6 - circle #2 (north-west)
	{   69,  -49, {AirportMovingDataFlag::NoSpeedClamp, AirportMovingDataFlag::SlowTurn}, Direction::N }, // 7 - circle #3 (south-west)
	{   69,    9, {AirportMovingDataFlag::NoSpeedClamp, AirportMovingDataFlag::SlowTurn}, Direction::N }, // 8 - circle #4 (south)
};

#undef AMD

///////////////////////////////////////////////////////////////////////
/////**********Movement Machine on Airports*********************///////
static const uint8_t _airport_entries_dummy[] = {0, 1, 2, 3};
static const AirportFTAbuildup _airport_fta_dummy[] = {
	{ 0, TO_ALL, {}, 3},
	{ 1, TO_ALL, {}, 0},
	{ 2, TO_ALL, {}, 1},
	{ 3, TO_ALL, {}, 2},
	{ MAX_ELEMENTS, TO_ALL, {}, 0 } // end marker. DO NOT REMOVE
};

/* First element of terminals array tells us how many depots there are (to know size of array)
 * this may be changed later when airports are moved to external file  */
static const HangarTileTable _airport_depots_country[] = { {{3, 0}, Direction::SE, 0} };
static const uint8_t _airport_terminal_country[] = {1, 2};
static const uint8_t _airport_entries_country[] = {16, 15, 18, 17};
static const AirportFTAbuildup _airport_fta_country[] = {
	{  0, HANGAR, AirportBlock::Nothing, 1 },
	{  1, TERMGROUP, AirportBlock::AirportBusy, 0 }, { 1, HANGAR, {}, 0 }, { 1, TERM1, AirportBlock::Term1, 2 }, { 1, TERM2, {}, 4 }, { 1, HELITAKEOFF, {}, 19 }, { 1, TO_ALL, {}, 6 },
	{  2, TERM1, AirportBlock::Term1, 1 },
	{  3, TERM2, AirportBlock::Term2, 5 },
	{  4, TERMGROUP, AirportBlock::AirportBusy, 0 }, { 4, TERM2, {}, 5 }, { 4, HANGAR, {}, 1 }, { 4, TAKEOFF, {}, 6 }, { 4, HELITAKEOFF, {}, 1 },
	{  5, TERMGROUP, AirportBlock::AirportBusy, 0 }, { 5, TERM2, AirportBlock::Term2, 3 }, { 5, TO_ALL, {}, 4 },
	{  6, TO_ALL, AirportBlock::AirportBusy, 7 },
	/* takeoff */
	{  7, TAKEOFF, AirportBlock::AirportBusy, 8 },
	{  8, STARTTAKEOFF, AirportBlock::Nothing, 9 },
	{  9, ENDTAKEOFF, AirportBlock::Nothing, 0 },
	/* landing */
	{ 10, FLYING, AirportBlock::Nothing, 15 }, { 10, LANDING, {}, 11 }, { 10, HELILANDING, {}, 20 },
	{ 11, LANDING, AirportBlock::AirportBusy, 12 },
	{ 12, TO_ALL, AirportBlock::AirportBusy, 13 },
	{ 13, ENDLANDING, AirportBlock::AirportBusy, 14 }, { 13, TERM2, {}, 5 }, { 13, TO_ALL, {}, 14 },
	{ 14, TO_ALL, AirportBlock::AirportBusy, 1 },
	/* In air */
	{ 15, TO_ALL, AirportBlock::Nothing, 16 },
	{ 16, TO_ALL, AirportBlock::Nothing, 17 },
	{ 17, TO_ALL, AirportBlock::Nothing, 18 },
	{ 18, TO_ALL, AirportBlock::Nothing, 10 },
	{ 19, HELITAKEOFF, AirportBlock::Nothing, 0 },
	{ 20, HELILANDING, AirportBlock::AirportBusy, 21 },
	{ 21, HELIENDLANDING, AirportBlock::AirportBusy, 1 },
	{ MAX_ELEMENTS, TO_ALL, {}, 0 } // end marker. DO NOT REMOVE
};

static const HangarTileTable _airport_depots_commuter[] = { {{4, 0}, Direction::SE, 0} };
static const uint8_t _airport_terminal_commuter[] = { 1, 3 };
static const uint8_t _airport_entries_commuter[] = {22, 21, 24, 23};
static const AirportFTAbuildup _airport_fta_commuter[] = {
	{  0, HANGAR, AirportBlock::Nothing, 1 }, { 0, HELITAKEOFF, AirportBlock::TaxiwayBusy, 1 }, { 0, TO_ALL, {}, 1 },
	{  1, TERMGROUP, AirportBlock::TaxiwayBusy, 0 }, { 1, HANGAR, {}, 0 }, { 1, TAKEOFF, {}, 11 }, { 1, TERM1, AirportBlock::TaxiwayBusy, 10 }, { 1, TERM2, AirportBlock::TaxiwayBusy, 10 }, { 1, TERM3, AirportBlock::TaxiwayBusy, 10 }, { 1, HELIPAD1, AirportBlock::TaxiwayBusy, 10 }, { 1, HELIPAD2, AirportBlock::TaxiwayBusy, 10 }, { 1, HELITAKEOFF, AirportBlock::TaxiwayBusy, 37 }, { 1, TO_ALL, {}, 0 },
	{  2, TERMGROUP, AirportBlock::AirportEntrance, 2 }, { 2, HANGAR, {}, 8 }, { 2, TERM1, {}, 8 }, { 2, TERM2, {}, 8 }, { 2, TERM3, {}, 8 }, { 2, HELIPAD1, {}, 8 }, { 2, HELIPAD2, {}, 8 }, { 2, HELITAKEOFF, {}, 8 }, { 2, TO_ALL, {}, 2 },
	{  3, TERM1, AirportBlock::Term1, 8 }, { 3, HANGAR, {}, 8 }, { 3, TAKEOFF, {}, 8 }, { 3, TO_ALL, {}, 3 },
	{  4, TERM2, AirportBlock::Term2, 9 }, { 4, HANGAR, {}, 9 }, { 4, TAKEOFF, {}, 9 }, { 4, TO_ALL, {}, 4 },
	{  5, TERM3, AirportBlock::Term3, 10 }, { 5, HANGAR, {}, 10 }, { 5, TAKEOFF, {}, 10 }, { 5, TO_ALL, {}, 5 },
	{  6, HELIPAD1, AirportBlock::Helipad1, 6 }, { 6, HANGAR, AirportBlock::TaxiwayBusy, 9 }, { 6, HELITAKEOFF, {}, 35 },
	{  7, HELIPAD2, AirportBlock::Helipad2, 7 }, { 7, HANGAR, AirportBlock::TaxiwayBusy, 10 }, { 7, HELITAKEOFF, {}, 36 },
	{  8, TERMGROUP, AirportBlock::TaxiwayBusy, 8 }, { 8, TAKEOFF, AirportBlock::TaxiwayBusy, 9 }, { 8, HANGAR, AirportBlock::TaxiwayBusy, 9 }, { 8, TERM1, AirportBlock::Term1, 3 }, { 8, TO_ALL, AirportBlock::TaxiwayBusy, 9 },
	{  9, TERMGROUP, AirportBlock::TaxiwayBusy, 9 }, { 9, TAKEOFF, AirportBlock::TaxiwayBusy, 10 }, { 9, HANGAR, AirportBlock::TaxiwayBusy, 10 }, { 9, TERM2, AirportBlock::Term2, 4 }, { 9, HELIPAD1, AirportBlock::Helipad1, 6 }, { 9, HELITAKEOFF, AirportBlock::Helipad1, 6 }, { 9, TERM1, AirportBlock::TaxiwayBusy, 8 }, { 9, TO_ALL, AirportBlock::TaxiwayBusy, 10 },
	{ 10, TERMGROUP, AirportBlock::TaxiwayBusy, 10 }, { 10, TERM3, AirportBlock::Term3, 5 }, { 10, HELIPAD1, {}, 9 }, { 10, HELIPAD2, AirportBlock::Helipad2, 7 }, { 10, HELITAKEOFF, {}, 1 }, { 10, TAKEOFF, AirportBlock::TaxiwayBusy, 1 }, { 10, HANGAR, AirportBlock::TaxiwayBusy, 1 }, { 10, TO_ALL, AirportBlock::TaxiwayBusy, 9 },
	{ 11, TO_ALL, AirportBlock::OutWay, 12 },
	/* takeoff */
	{ 12, TAKEOFF, AirportBlock::RunwayInOut, 13 },
	{ 13, TO_ALL, AirportBlock::RunwayInOut, 14 },
	{ 14, STARTTAKEOFF, AirportBlock::RunwayInOut, 15 },
	{ 15, ENDTAKEOFF, AirportBlock::Nothing, 0 },
	/* landing */
	{ 16, FLYING, AirportBlock::Nothing, 21 }, { 16, LANDING, AirportBlock::InWay, 17 }, { 16, HELILANDING, {}, 25 },
	{ 17, LANDING, AirportBlock::RunwayInOut, 18 },
	{ 18, TO_ALL, AirportBlock::RunwayInOut, 19 },
	{ 19, TO_ALL, AirportBlock::RunwayInOut, 20 },
	{ 20, ENDLANDING, AirportBlock::InWay, 2 },
	/* In Air */
	{ 21, TO_ALL, AirportBlock::Nothing, 22 },
	{ 22, TO_ALL, AirportBlock::Nothing, 23 },
	{ 23, TO_ALL, AirportBlock::Nothing, 24 },
	{ 24, TO_ALL, AirportBlock::Nothing, 16 },
	/* Helicopter -- stay in air in special place as a buffer to choose from helipads */
	{ 25, HELILANDING, AirportBlock::PreHelipad, 26 },
	{ 26, HELIENDLANDING, AirportBlock::PreHelipad, 26 }, { 26, HELIPAD1, {}, 27 }, { 26, HELIPAD2, {}, 28 }, { 26, HANGAR, {}, 33 },
	{ 27, TO_ALL, AirportBlock::Nothing, 29 }, // helipad1 approach
	{ 28, TO_ALL, AirportBlock::Nothing, 30 },
	/* landing */
	{ 29, TERMGROUP, AirportBlock::Nothing, 0 }, { 29, HELIPAD1, AirportBlock::Helipad1, 6 },
	{ 30, TERMGROUP, AirportBlock::Nothing, 0 }, { 30, HELIPAD2, AirportBlock::Helipad2, 7 },
	/* Helicopter -- takeoff */
	{ 31, HELITAKEOFF, AirportBlock::Nothing, 0 },
	{ 32, HELITAKEOFF, AirportBlock::Nothing, 0 },
	{ 33, TO_ALL, AirportBlock::TaxiwayBusy, 34 }, // need to go to hangar when waiting in air
	{ 34, TO_ALL, AirportBlock::TaxiwayBusy, 1 },
	{ 35, TO_ALL, AirportBlock::Helipad1, 31 },
	{ 36, TO_ALL, AirportBlock::Helipad2, 32 },
	{ 37, HELITAKEOFF, AirportBlock::Nothing, 0 },
	{ MAX_ELEMENTS, TO_ALL, {}, 0 } // end marker. DO NOT REMOVE
};

static const HangarTileTable _airport_depots_city[] = { {{5, 0}, Direction::SE, 0} };
static const uint8_t _airport_terminal_city[] = { 1, 3 };
static const uint8_t _airport_entries_city[] = {26, 29, 27, 28};
static const AirportFTAbuildup _airport_fta_city[] = {
	{  0, HANGAR, AirportBlock::Nothing, 1 }, { 0, TAKEOFF, AirportBlock::OutWay, 1 }, { 0, TO_ALL, {}, 1 },
	{  1, TERMGROUP, AirportBlock::TaxiwayBusy, 0 }, { 1, HANGAR, {}, 0 }, { 1, TERM2, {}, 6 }, { 1, TERM3, {}, 6 }, { 1, TO_ALL, {}, 7 }, // for all else, go to 7
	{  2, TERM1, AirportBlock::Term1, 7 }, { 2, TAKEOFF, AirportBlock::OutWay, 7 }, { 2, TO_ALL, {}, 7 },
	{  3, TERM2, AirportBlock::Term2, 5 }, { 3, TAKEOFF, AirportBlock::OutWay, 6 }, { 3, TO_ALL, {}, 6 },
	{  4, TERM3, AirportBlock::Term3, 5 }, { 4, TAKEOFF, AirportBlock::OutWay, 5 }, { 4, TO_ALL, {}, 5 },
	{  5, TERMGROUP, AirportBlock::TaxiwayBusy, 0 }, { 5, TERM2, AirportBlock::Term2, 3 }, { 5, TERM3, AirportBlock::Term3, 4 }, { 5, TO_ALL, {}, 6 },
	{  6, TERMGROUP, AirportBlock::TaxiwayBusy, 0 }, { 6, TERM2, AirportBlock::Term2, 3 }, { 6, TERM3, {}, 5 }, { 6, HANGAR, {}, 1 }, { 6, TO_ALL, {}, 7 },
	{  7, TERMGROUP, AirportBlock::TaxiwayBusy, 0 }, { 7, TERM1, AirportBlock::Term1, 2 }, { 7, TAKEOFF, AirportBlock::OutWay, 8 }, { 7, HELITAKEOFF, {}, 22 }, { 7, HANGAR, {}, 1 }, { 7, TO_ALL, {}, 6 },
	{  8, TO_ALL, AirportBlock::OutWay, 9 },
	{  9, TO_ALL, AirportBlock::RunwayInOut, 10 },
	/* takeoff */
	{ 10, TAKEOFF, AirportBlock::RunwayInOut, 11 },
	{ 11, STARTTAKEOFF, AirportBlock::Nothing, 12 },
	{ 12, ENDTAKEOFF, AirportBlock::Nothing, 0 },
	/* landing */
	{ 13, FLYING, AirportBlock::Nothing, 18 }, { 13, LANDING, {}, 14 }, { 13, HELILANDING, {}, 23 },
	{ 14, LANDING, AirportBlock::RunwayInOut, 15 },
	{ 15, TO_ALL, AirportBlock::RunwayInOut, 17 },
	{ 16, TO_ALL, AirportBlock::RunwayInOut, 17 }, // not used, left for compatibility
	{ 17, ENDLANDING, AirportBlock::InWay, 7 },
	/* In Air */
	{ 18, TO_ALL, AirportBlock::Nothing, 25 },
	{ 19, TO_ALL, AirportBlock::Nothing, 20 },
	{ 20, TO_ALL, AirportBlock::Nothing, 21 },
	{ 21, TO_ALL, AirportBlock::Nothing, 13 },
	/* helicopter */
	{ 22, HELITAKEOFF, AirportBlock::Nothing, 0 },
	{ 23, HELILANDING, AirportBlock::InWay, 24 },
	{ 24, HELIENDLANDING, AirportBlock::InWay, 17 },
	{ 25, TO_ALL, AirportBlock::Nothing, 20},
	{ 26, TO_ALL, AirportBlock::Nothing, 19},
	{ 27, TO_ALL, AirportBlock::Nothing, 28},
	{ 28, TO_ALL, AirportBlock::Nothing, 19},
	{ 29, TO_ALL, AirportBlock::Nothing, 26},
	{ MAX_ELEMENTS, TO_ALL, {}, 0 } // end marker. DO NOT REMOVE
};

static const HangarTileTable _airport_depots_metropolitan[] = { {{5, 0}, Direction::SE, 0} };
static const uint8_t _airport_terminal_metropolitan[] = { 1, 3 };
static const uint8_t _airport_entries_metropolitan[] = {20, 19, 22, 21};
static const AirportFTAbuildup _airport_fta_metropolitan[] = {
	{  0, HANGAR, AirportBlock::Nothing, 1 },
	{  1, TERMGROUP, AirportBlock::TaxiwayBusy, 0 }, { 1, HANGAR, {}, 0 }, { 1, TERM2, {}, 6 }, { 1, TERM3, {}, 6 }, { 1, TO_ALL, {}, 7 }, // for all else, go to 7
	{  2, TERM1, AirportBlock::Term1, 7 },
	{  3, TERM2, AirportBlock::Term2, 6 },
	{  4, TERM3, AirportBlock::Term3, 5 },
	{  5, TERMGROUP, AirportBlock::TaxiwayBusy, 0 }, { 5, TERM2, AirportBlock::Term2, 3 }, { 5, TERM3, AirportBlock::Term3, 4 }, { 5, TO_ALL, {}, 6 },
	{  6, TERMGROUP, AirportBlock::TaxiwayBusy, 0 }, { 6, TERM2, AirportBlock::Term2, 3 }, { 6, TERM3, {}, 5 }, { 6, HANGAR, {}, 1 }, { 6, TO_ALL, {}, 7 },
	{  7, TERMGROUP, AirportBlock::TaxiwayBusy, 0 }, { 7, TERM1, AirportBlock::Term1, 2 }, { 7, TAKEOFF, {}, 8 }, { 7, HELITAKEOFF, {}, 23 }, { 7, HANGAR, {}, 1 }, { 7, TO_ALL, {}, 6 },
	{  8, 0, AirportBlock::OutWay, 9 },
	{  9, 0, AirportBlock::RunwayOut, 10 },
	/* takeoff */
	{ 10, TAKEOFF, AirportBlock::RunwayOut, 11 },
	{ 11, STARTTAKEOFF, AirportBlock::Nothing, 12 },
	{ 12, ENDTAKEOFF, AirportBlock::Nothing, 0 },
	/* landing */
	{ 13, FLYING, AirportBlock::Nothing, 19 }, { 13, LANDING, {}, 14 }, { 13, HELILANDING, {}, 25 },
	{ 14, LANDING, AirportBlock::RunwayIn, 15 },
	{ 15, TO_ALL, AirportBlock::RunwayIn, 16 },
	{ 16, TERMGROUP, AirportBlock::RunwayIn, 0 }, { 16, ENDLANDING, AirportBlock::InWay, 17 },
	{ 17, TERMGROUP, AirportBlock::RunwayOut, 0 }, { 17, ENDLANDING, AirportBlock::InWay, 18 },
	{ 18, ENDLANDING, AirportBlock::InWay, 27 },
	/* In Air */
	{ 19, TO_ALL, AirportBlock::Nothing, 20 },
	{ 20, TO_ALL, AirportBlock::Nothing, 21 },
	{ 21, TO_ALL, AirportBlock::Nothing, 22 },
	{ 22, TO_ALL, AirportBlock::Nothing, 13 },
	/* helicopter */
	{ 23, TO_ALL, AirportBlock::Nothing, 24 },
	{ 24, HELITAKEOFF, AirportBlock::Nothing, 0 },
	{ 25, HELILANDING, AirportBlock::InWay, 26 },
	{ 26, HELIENDLANDING, AirportBlock::InWay, 18 },
	{ 27, TERMGROUP, AirportBlock::TaxiwayBusy, 27 }, { 27, TERM1, AirportBlock::Term1, 2 }, { 27, TO_ALL, {}, 7 },
	{ MAX_ELEMENTS, TO_ALL, {}, 0 } // end marker. DO NOT REMOVE
};

static const HangarTileTable _airport_depots_international[] = { {{0, 3}, Direction::SE, 0}, {{6, 1}, Direction::SE, 1} };
static const uint8_t _airport_terminal_international[] = { 2, 3, 3 };
static const uint8_t _airport_entries_international[] = { 38, 37, 40, 39 };
static const AirportFTAbuildup _airport_fta_international[] = {
	{  0, HANGAR, AirportBlock::Nothing, 2 }, { 0, TERMGROUP, AirportBlock::TermGroup1, 0 }, { 0, TERMGROUP, AirportBlock::TermGroup2Enter1, 1 }, { 0, HELITAKEOFF, AirportBlock::AirportEntrance, 2 }, { 0, TO_ALL, {}, 2 },
	{  1, HANGAR, AirportBlock::Nothing, 3 }, { 1, TERMGROUP, AirportBlock::Hangar2Area, 1 }, { 1, HELITAKEOFF, AirportBlock::Hangar2Area, 3 }, { 1, TO_ALL, {}, 3 },
	{  2, TERMGROUP, AirportBlock::AirportEntrance, 0 }, { 2, HANGAR, {}, 0 }, { 2, TERM4, {}, 12 }, { 2, TERM5, {}, 12 }, { 2, TERM6, {}, 12 }, { 2, HELIPAD1, {}, 12 }, { 2, HELIPAD2, {}, 12 }, { 2, HELITAKEOFF, {}, 51 }, { 2, TO_ALL, {}, 23 },
	{  3, TERMGROUP, AirportBlock::Hangar2Area, 0 }, { 3, HANGAR, {}, 1 }, { 3, HELITAKEOFF, {}, 52 }, { 3, TO_ALL, {}, 18 },
	{  4, TERM1, AirportBlock::Term1, 23 }, { 4, HANGAR, AirportBlock::AirportEntrance, 23 }, { 4, TO_ALL, {}, 23 },
	{  5, TERM2, AirportBlock::Term2, 24 }, { 5, HANGAR, AirportBlock::AirportEntrance, 24 }, { 5, TO_ALL, {}, 24 },
	{  6, TERM3, AirportBlock::Term3, 25 }, { 6, HANGAR, AirportBlock::AirportEntrance, 25 }, { 6, TO_ALL, {}, 25 },
	{  7, TERM4, AirportBlock::Term4, 16 }, { 7, HANGAR, AirportBlock::Hangar2Area, 16 }, { 7, TO_ALL, {}, 16 },
	{  8, TERM5, AirportBlock::Term5, 17 }, { 8, HANGAR, AirportBlock::Hangar2Area, 17 }, { 8, TO_ALL, {}, 17 },
	{  9, TERM6, AirportBlock::Term6, 18 }, { 9, HANGAR, AirportBlock::Hangar2Area, 18 }, { 9, TO_ALL, {}, 18 },
	{ 10, HELIPAD1, AirportBlock::Helipad1, 10 }, { 10, HANGAR, AirportBlock::Hangar2Area, 16 }, { 10, HELITAKEOFF, {}, 47 },
	{ 11, HELIPAD2, AirportBlock::Helipad2, 11 }, { 11, HANGAR, AirportBlock::Hangar2Area, 17 }, { 11, HELITAKEOFF, {}, 48 },
	{ 12, TO_ALL, AirportBlock::TermGroup2Enter1, 13 },
	{ 13, TO_ALL, AirportBlock::TermGroup2Enter1, 14 },
	{ 14, TO_ALL, AirportBlock::TermGroup2Enter2, 15 },
	{ 15, TO_ALL, AirportBlock::TermGroup2Enter2, 16 },
	{ 16, TERMGROUP, AirportBlock::TermGroup2, 0 }, { 16, TERM4, AirportBlock::Term4, 7 }, { 16, HELIPAD1, AirportBlock::Helipad1, 10 }, { 16, HELITAKEOFF, AirportBlock::Helipad1, 10 }, { 16, TO_ALL, {}, 17 },
	{ 17, TERMGROUP, AirportBlock::TermGroup2, 0 }, { 17, TERM5, AirportBlock::Term5, 8 }, { 17, TERM4, {}, 16 }, { 17, HELIPAD1, {}, 16 }, { 17, HELIPAD2, AirportBlock::Helipad2, 11 }, { 17, HELITAKEOFF, AirportBlock::Helipad2, 11 }, { 17, TO_ALL, {}, 18 },
	{ 18, TERMGROUP, AirportBlock::TermGroup2, 0 }, { 18, TERM6, AirportBlock::Term6, 9 }, { 18, TAKEOFF, {}, 19 }, { 18, HANGAR, AirportBlock::Hangar2Area, 3 }, { 18, TO_ALL, {}, 17 },
	{ 19, TO_ALL, AirportBlock::TermGroup2Exit1, 20 },
	{ 20, TO_ALL, AirportBlock::TermGroup2Exit1, 21 },
	{ 21, TO_ALL, AirportBlock::TermGroup2Exit2, 22 },
	{ 22, TO_ALL, AirportBlock::TermGroup2Exit2, 26 },
	{ 23, TERMGROUP, AirportBlock::TermGroup1, 0 }, { 23, TERM1, AirportBlock::Term1, 4 }, { 23, HANGAR, AirportBlock::AirportEntrance, 2 }, { 23, TO_ALL, {}, 24 },
	{ 24, TERMGROUP, AirportBlock::TermGroup1, 0 }, { 24, TERM2, AirportBlock::Term2, 5 }, { 24, TERM1, {}, 23 }, { 24, HANGAR, {}, 23 }, { 24, TO_ALL, {}, 25 },
	{ 25, TERMGROUP, AirportBlock::TermGroup1, 0 }, { 25, TERM3, AirportBlock::Term3, 6 }, { 25, TAKEOFF, {}, 26 }, { 25, TO_ALL, {}, 24 },
	{ 26, TERMGROUP, AirportBlock::TaxiwayBusy, 0 }, { 26, TAKEOFF, {}, 27 }, { 26, TO_ALL, {}, 25 },
	{ 27, TO_ALL, AirportBlock::OutWay, 28 },
	/* takeoff */
	{ 28, TAKEOFF, AirportBlock::OutWay, 29 },
	{ 29, TO_ALL, AirportBlock::RunwayOut, 30 },
	{ 30, STARTTAKEOFF, AirportBlock::Nothing, 31 },
	{ 31, ENDTAKEOFF, AirportBlock::Nothing, 0 },
	/* landing */
	{ 32, FLYING, AirportBlock::Nothing, 37 }, { 32, LANDING, {}, 33 }, { 32, HELILANDING, {}, 41 },
	{ 33, LANDING, AirportBlock::RunwayIn, 34 },
	{ 34, TO_ALL, AirportBlock::RunwayIn, 35 },
	{ 35, TO_ALL, AirportBlock::RunwayIn, 36 },
	{ 36, ENDLANDING, AirportBlock::InWay, 36 }, { 36, TERMGROUP, AirportBlock::TermGroup1, 0 }, { 36, TERMGROUP, AirportBlock::TermGroup2Enter1, 1 }, { 36, TERM4, {}, 12 }, { 36, TERM5, {}, 12 }, { 36, TERM6, {}, 12 }, { 36, TO_ALL, {}, 2 },
	/* In Air */
	{ 37, TO_ALL, AirportBlock::Nothing, 38 },
	{ 38, TO_ALL, AirportBlock::Nothing, 39 },
	{ 39, TO_ALL, AirportBlock::Nothing, 40 },
	{ 40, TO_ALL, AirportBlock::Nothing, 32 },
	/* Helicopter -- stay in air in special place as a buffer to choose from helipads */
	{ 41, HELILANDING, AirportBlock::PreHelipad, 42 },
	{ 42, HELIENDLANDING, AirportBlock::PreHelipad, 42 }, { 42, HELIPAD1, {}, 43 }, { 42, HELIPAD2, {}, 44 }, { 42, HANGAR, {}, 49 },
	{ 43, TO_ALL, AirportBlock::Nothing, 45 },
	{ 44, TO_ALL, AirportBlock::Nothing, 46 },
	/* landing */
	{ 45, TERMGROUP, AirportBlock::Nothing, 0 }, { 45, HELIPAD1, AirportBlock::Helipad1, 10 },
	{ 46, TERMGROUP, AirportBlock::Nothing, 0 }, { 46, HELIPAD2, AirportBlock::Helipad2, 11 },
	/* Helicopter -- takeoff */
	{ 47, HELITAKEOFF, AirportBlock::Nothing, 0 },
	{ 48, HELITAKEOFF, AirportBlock::Nothing, 0 },
	{ 49, TO_ALL, AirportBlock::Hangar2Area, 50 }, // need to go to hangar when waiting in air
	{ 50, TO_ALL, AirportBlock::Hangar2Area, 3 },
	{ 51, HELITAKEOFF, AirportBlock::Nothing, 0 },
	{ 52, HELITAKEOFF, AirportBlock::Nothing, 0 },
	{ MAX_ELEMENTS, TO_ALL, {}, 0 } // end marker. DO NOT REMOVE
};

/* intercontinental */
static const HangarTileTable _airport_depots_intercontinental[] = { {{0, 5}, Direction::SE, 0}, {{8, 4}, Direction::SE, 1} };
static const uint8_t _airport_terminal_intercontinental[] = { 2, 4, 4 };
static const uint8_t _airport_entries_intercontinental[] = { 44, 43, 46, 45 };
static const AirportFTAbuildup _airport_fta_intercontinental[] = {
	{  0, HANGAR, AirportBlock::Nothing, 2 }, { 0, TERMGROUP, {AirportBlock::Hangar1Area, AirportBlock::TermGroup1}, 0 }, { 0, TERMGROUP, {AirportBlock::Hangar1Area, AirportBlock::TermGroup1}, 1 }, { 0, TAKEOFF, {AirportBlock::Hangar1Area, AirportBlock::TermGroup1}, 2 }, { 0, TO_ALL, {}, 2 },
	{  1, HANGAR, AirportBlock::Nothing, 3 }, { 1, TERMGROUP, AirportBlock::Hangar2Area, 1 }, { 1, TERMGROUP, AirportBlock::Hangar2Area, 0 }, { 1, TO_ALL, {}, 3 },
	{  2, TERMGROUP, AirportBlock::Hangar1Area, 0 }, { 2, TERMGROUP, AirportBlock::TermGroup1, 0 }, { 2, TERMGROUP, AirportBlock::TermGroup1, 1 }, { 2, HANGAR, {}, 0 }, { 2, TAKEOFF, AirportBlock::TermGroup1, 27 }, { 2, TERM5, {}, 26 }, { 2, TERM6, {}, 26 }, { 2, TERM7, {}, 26 }, { 2, TERM8, {}, 26 }, { 2, HELIPAD1, {}, 26 }, { 2, HELIPAD2, {}, 26 }, { 2, HELITAKEOFF, {}, 74 }, { 2, TO_ALL, {}, 27 },
	{  3, TERMGROUP, AirportBlock::Hangar2Area, 0 }, { 3, HANGAR, {}, 1 }, { 3, HELITAKEOFF, {}, 75 }, {3, TAKEOFF, {}, 59}, { 3, TO_ALL, {}, 20 },
	{  4, TERM1, AirportBlock::Term1, 26 }, { 4, HANGAR, {AirportBlock::Hangar1Area, AirportBlock::TermGroup1}, 26 }, { 4, TO_ALL, {}, 26 },
	{  5, TERM2, AirportBlock::Term2, 27 }, { 5, HANGAR, {AirportBlock::Hangar1Area, AirportBlock::TermGroup1}, 27 }, { 5, TO_ALL, {}, 27 },
	{  6, TERM3, AirportBlock::Term3, 28 }, { 6, HANGAR, {AirportBlock::Hangar1Area, AirportBlock::TermGroup1}, 28 }, { 6, TO_ALL, {}, 28 },
	{  7, TERM4, AirportBlock::Term4, 29 }, { 7, HANGAR, {AirportBlock::Hangar1Area, AirportBlock::TermGroup1}, 29 }, { 7, TO_ALL, {}, 29 },
	{  8, TERM5, AirportBlock::Term5, 18 }, { 8, HANGAR, AirportBlock::Hangar2Area, 18 }, { 8, TO_ALL, {}, 18 },
	{  9, TERM6, AirportBlock::Term6, 19 }, { 9, HANGAR, AirportBlock::Hangar2Area, 19 }, { 9, TO_ALL, {}, 19 },
	{ 10, TERM7, AirportBlock::Term7, 20 }, { 10, HANGAR, AirportBlock::Hangar2Area, 20 }, { 10, TO_ALL, {}, 20 },
	{ 11, TERM8, AirportBlock::Term8, 21 }, { 11, HANGAR, AirportBlock::Hangar2Area, 21 }, { 11, TO_ALL, {}, 21 },
	{ 12, HELIPAD1, AirportBlock::Helipad1, 12 }, { 12, HANGAR, {}, 70 }, { 12, HELITAKEOFF, {}, 72 },
	{ 13, HELIPAD2, AirportBlock::Helipad2, 13 }, { 13, HANGAR, {}, 71 }, { 13, HELITAKEOFF, {}, 73 },
	{ 14, TO_ALL, AirportBlock::TermGroup2Enter1, 15 },
	{ 15, TO_ALL, AirportBlock::TermGroup2Enter1, 16 },
	{ 16, TO_ALL, AirportBlock::TermGroup2Enter2, 17 },
	{ 17, TO_ALL, AirportBlock::TermGroup2Enter2, 18 },
	{ 18, TERMGROUP, AirportBlock::TermGroup2, 0 }, { 18, TERM5, AirportBlock::Term5, 8 }, { 18, TAKEOFF, {}, 19 }, { 18, HELITAKEOFF, AirportBlock::Helipad1, 19 }, { 18, TO_ALL, AirportBlock::TermGroup2Exit1, 19 },
	{ 19, TERMGROUP, AirportBlock::TermGroup2, 0 }, { 19, TERM6, AirportBlock::Term6, 9 }, { 19, TERM5, {}, 18 }, { 19, TAKEOFF, {}, 57 }, { 19, HELITAKEOFF, AirportBlock::Helipad1, 20 }, { 19, TO_ALL, AirportBlock::TermGroup2Exit1, 20 }, // add exit to runway out 2
	{ 20, TERMGROUP, AirportBlock::TermGroup2, 0 }, { 20, TERM7, AirportBlock::Term7, 10 }, { 20, TERM5, {}, 19 }, { 20, TERM6, {}, 19 }, { 20, HANGAR, AirportBlock::Hangar2Area, 3 }, { 20, TAKEOFF, {}, 19 }, { 20, TO_ALL, AirportBlock::TermGroup2Exit1, 21 },
	{ 21, TERMGROUP, AirportBlock::TermGroup2, 0 }, { 21, TERM8, AirportBlock::Term8, 11 }, { 21, HANGAR, AirportBlock::Hangar2Area, 20 }, { 21, TERM5, {}, 20 }, { 21, TERM6, {}, 20 }, { 21, TERM7, {}, 20 }, { 21, TAKEOFF, {}, 20 }, { 21, TO_ALL, AirportBlock::TermGroup2Exit1, 22 },
	{ 22, TERMGROUP, AirportBlock::TermGroup2, 0 }, { 22, HANGAR, {}, 21 }, { 22, TERM5, {}, 21 }, { 22, TERM6, {}, 21 }, { 22, TERM7, {}, 21 }, { 22, TERM8, {}, 21 }, { 22, TAKEOFF, {}, 21 }, { 22, TO_ALL, {}, 23 },
	{ 23, TO_ALL, AirportBlock::TermGroup2Exit1, 70 },
	{ 24, TO_ALL, AirportBlock::TermGroup2Exit2, 25 },
	{ 25, TERMGROUP, AirportBlock::TermGroup2Exit2, 0 }, { 25, HANGAR, {AirportBlock::Hangar1Area, AirportBlock::TermGroup1}, 29 }, { 25, TO_ALL, {}, 29 },
	{ 26, TERMGROUP, AirportBlock::TermGroup1, 0 }, { 26, TERM1, AirportBlock::Term1, 4 }, { 26, HANGAR, AirportBlock::Hangar1Area, 27 }, { 26, TERM5, AirportBlock::TermGroup2Enter1, 14 }, { 26, TERM6, AirportBlock::TermGroup2Enter1, 14 }, { 26, TERM7, AirportBlock::TermGroup2Enter1, 14 }, { 26, TERM8, AirportBlock::TermGroup2Enter1, 14 }, { 26, HELIPAD1, AirportBlock::TermGroup2Enter1, 14 }, { 26, HELIPAD2, AirportBlock::TermGroup2Enter1, 14 }, { 26, HELITAKEOFF, AirportBlock::TermGroup2Enter1, 14 }, { 26, TO_ALL, {}, 27 },
	{ 27, TERMGROUP, AirportBlock::TermGroup1, 0 }, { 27, TERM2, AirportBlock::Term2, 5 }, { 27, HANGAR, AirportBlock::Hangar1Area, 2 }, { 27, TERM1, {}, 26 }, { 27, TERM5, {}, 26 }, { 27, TERM6, {}, 26 }, { 27, TERM7, {}, 26 }, { 27, TERM8, {}, 26 }, { 27, HELIPAD1, {}, 14 }, { 27, HELIPAD2, {}, 14 }, { 27, TO_ALL, {}, 28 },
	{ 28, TERMGROUP, AirportBlock::TermGroup1, 0 }, { 28, TERM3, AirportBlock::Term3, 6 }, { 28, HANGAR, AirportBlock::Hangar1Area, 27 }, { 28, TERM1, {}, 27 }, { 28, TERM2, {}, 27 }, { 28, TERM4, {}, 29 }, { 28, TERM5, {}, 14 }, { 28, TERM6, {}, 14 }, { 28, TERM7, {}, 14 }, { 28, TERM8, {}, 14 }, { 28, HELIPAD1, {}, 14 }, { 28, HELIPAD2, {}, 14 }, { 28, TO_ALL, {}, 29 },
	{ 29, TERMGROUP, AirportBlock::TermGroup1, 0 }, { 29, TERM4, AirportBlock::Term4, 7 }, { 29, HANGAR, AirportBlock::Hangar1Area, 27 }, { 29, TAKEOFF, {}, 30 }, { 29, TO_ALL, {}, 28 },
	{ 30, TO_ALL, AirportBlock::OutWay3, 31 },
	{ 31, TO_ALL, AirportBlock::OutWay, 32 },
	/* takeoff */
	{ 32, TAKEOFF, AirportBlock::RunwayOut, 33 },
	{ 33, TO_ALL, AirportBlock::RunwayOut, 34 },
	{ 34, STARTTAKEOFF, AirportBlock::Nothing, 35 },
	{ 35, ENDTAKEOFF, AirportBlock::Nothing, 0 },
	/* landing */
	{ 36, TO_ALL, {}, 0 },
	{ 37, LANDING, AirportBlock::RunwayIn, 38 },
	{ 38, TO_ALL, AirportBlock::RunwayIn, 39 },
	{ 39, TO_ALL, AirportBlock::RunwayIn, 40 },
	{ 40, ENDLANDING, AirportBlock::RunwayIn, 41 },
	{ 41, TO_ALL, AirportBlock::InWay, 42 },
	{ 42, TERMGROUP, AirportBlock::InWay, 0 }, { 42, TERMGROUP, AirportBlock::TermGroup1, 0 }, { 42, TERMGROUP, AirportBlock::TermGroup1, 1 }, { 42, HANGAR, {}, 2 }, { 42, TO_ALL, {}, 26 },
	/* In Air */
	{ 43, TO_ALL, {}, 44 },
	{ 44, FLYING, {}, 45 }, { 44, HELILANDING, {}, 47 }, { 44, LANDING, {}, 69 }, { 44, TO_ALL, {}, 45 },
	{ 45, TO_ALL, {}, 46 },
	{ 46, FLYING, {}, 43 }, { 46, LANDING, {}, 76 }, { 46, TO_ALL, {}, 43 },
	/* Helicopter -- stay in air in special place as a buffer to choose from helipads */
	{ 47, HELILANDING, AirportBlock::PreHelipad, 48 },
	{ 48, HELIENDLANDING, AirportBlock::PreHelipad, 48 }, { 48, HELIPAD1, {}, 49 }, { 48, HELIPAD2, {}, 50 }, { 48, HANGAR, {}, 55 },
	{ 49, TO_ALL, AirportBlock::Nothing, 51 },
	{ 50, TO_ALL, AirportBlock::Nothing, 52 },
	/* landing */
	{ 51, TERMGROUP, AirportBlock::Nothing, 0 }, { 51, HELIPAD1, AirportBlock::Helipad1, 12 }, { 51, HANGAR, {}, 55 }, { 51, TO_ALL, {}, 12 },
	{ 52, TERMGROUP, AirportBlock::Nothing, 0 }, { 52, HELIPAD2, AirportBlock::Helipad2, 13 }, { 52, HANGAR, {}, 55 }, { 52, TO_ALL, {}, 13 },
	/* Helicopter -- takeoff */
	{ 53, HELITAKEOFF, AirportBlock::Nothing, 0 },
	{ 54, HELITAKEOFF, AirportBlock::Nothing, 0 },
	{ 55, TO_ALL, AirportBlock::Hangar2Area, 56 }, // need to go to hangar when waiting in air
	{ 56, TO_ALL, AirportBlock::Hangar2Area, 3 },
	/* runway 2 out support */
	{ 57, TERMGROUP, AirportBlock::OutWay2, 0 }, { 57, TAKEOFF, {}, 58 }, { 57, TO_ALL, {}, 58 },
	{ 58, TO_ALL, AirportBlock::OutWay2, 59 },
	{ 59, TAKEOFF, AirportBlock::RunwayOut2, 60 }, // takeoff
	{ 60, TO_ALL, AirportBlock::RunwayOut2, 61 },
	{ 61, STARTTAKEOFF, AirportBlock::Nothing, 62 },
	{ 62, ENDTAKEOFF, AirportBlock::Nothing, 0 },
	/* runway 2 in support */
	{ 63, LANDING, AirportBlock::RunwayIn2, 64 },
	{ 64, TO_ALL, AirportBlock::RunwayIn2, 65 },
	{ 65, TO_ALL, AirportBlock::RunwayIn2, 66 },
	{ 66, ENDLANDING, AirportBlock::RunwayIn2, 0 }, { 66, TERMGROUP, {}, 1 }, { 66, TERMGROUP, {}, 0 }, { 66, TO_ALL, {}, 67 },
	{ 67, TO_ALL, AirportBlock::InWay2, 68 },
	{ 68, TERMGROUP, AirportBlock::InWay2, 0 }, { 68, TERMGROUP, AirportBlock::TermGroup2, 1 }, { 68, TERMGROUP, AirportBlock::TermGroup1, 0 }, { 68, HANGAR, AirportBlock::Hangar2Area, 22 }, { 68, TO_ALL, {}, 22 },
	{ 69, TERMGROUP, AirportBlock::RunwayIn2, 0 }, { 69, TO_ALL, AirportBlock::RunwayIn2, 63 },
	{ 70, TERMGROUP, AirportBlock::TermGroup2Exit1, 0 }, { 70, HELIPAD1, AirportBlock::Helipad1, 12 }, { 70, HELITAKEOFF, AirportBlock::Helipad1, 12 }, { 70, TO_ALL, {}, 71 },
	{ 71, TERMGROUP, AirportBlock::TermGroup2Exit1, 0 }, { 71, HELIPAD2, AirportBlock::Helipad2, 13 }, { 71, HELITAKEOFF, AirportBlock::Helipad1, 12 }, { 71, TO_ALL, {}, 24 },
	{ 72, TO_ALL, AirportBlock::Helipad1, 53 },
	{ 73, TO_ALL, AirportBlock::Helipad2, 54 },
	{ 74, HELITAKEOFF, AirportBlock::Nothing, 0 },
	{ 75, HELITAKEOFF, AirportBlock::Nothing, 0 },
	{ 76, TERMGROUP, AirportBlock::RunwayIn, 0 }, { 76, TO_ALL, AirportBlock::RunwayIn, 37 },
	{ MAX_ELEMENTS, TO_ALL, {}, 0 } // end marker. DO NOT REMOVE
};


/* heliports, oilrigs don't have depots */
static const uint8_t _airport_entries_heliport[] = { 7, 7, 7, 7 };
static const AirportFTAbuildup _airport_fta_heliport[] = {
	{ 0, HELIPAD1, AirportBlock::Helipad1, 1 },
	{ 1, HELITAKEOFF, AirportBlock::Nothing, 0 }, // takeoff
	{ 2, TERMGROUP, AirportBlock::AirportBusy, 0 }, { 2, HELILANDING, {}, 3 }, { 2, HELITAKEOFF, {}, 1 },
	{ 3, HELILANDING, AirportBlock::AirportBusy, 4 },
	{ 4, HELIENDLANDING, AirportBlock::AirportBusy, 4 }, { 4, HELIPAD1, AirportBlock::Helipad1, 0 }, { 4, HELITAKEOFF, {}, 2 },
	/* In Air */
	{ 5, TO_ALL, AirportBlock::Nothing, 6 },
	{ 6, TO_ALL, AirportBlock::Nothing, 7 },
	{ 7, TO_ALL, AirportBlock::Nothing, 8 },
	{ 8, FLYING, AirportBlock::Nothing, 5 }, { 8, HELILANDING, AirportBlock::Helipad1, 2 }, // landing
	{ MAX_ELEMENTS, TO_ALL, {}, 0 } // end marker. DO NOT REMOVE
};
#define _airport_entries_oilrig _airport_entries_heliport
#define _airport_fta_oilrig _airport_fta_heliport

/* helidepots */
static const HangarTileTable _airport_depots_helidepot[] = { {{1, 0}, Direction::SE, 0} };
static const uint8_t _airport_entries_helidepot[] = { 4, 4, 4, 4 };
static const AirportFTAbuildup _airport_fta_helidepot[] = {
	{  0, HANGAR, AirportBlock::Nothing, 1 },
	{  1, TERMGROUP, AirportBlock::Hangar2Area, 0 }, { 1, HANGAR, {}, 0 }, { 1, HELIPAD1, AirportBlock::Helipad1, 14 }, { 1, HELITAKEOFF, {}, 15 }, { 1, TO_ALL, {}, 0 },
	{  2, FLYING, AirportBlock::Nothing, 3 }, { 2, HELILANDING, AirportBlock::PreHelipad, 7 }, { 2, HANGAR, {}, 12 }, { 2, HELITAKEOFF, AirportBlock::Nothing, 16 },
	/* In Air */
	{  3, TO_ALL, AirportBlock::Nothing, 4 },
	{  4, TO_ALL, AirportBlock::Nothing, 5 },
	{  5, TO_ALL, AirportBlock::Nothing, 6 },
	{  6, TO_ALL, AirportBlock::Nothing, 2 },
	/* Helicopter -- stay in air in special place as a buffer to choose from helipads */
	{  7, HELILANDING, AirportBlock::PreHelipad, 8 },
	{  8, HELIENDLANDING, AirportBlock::PreHelipad, 8 }, { 8, HELIPAD1, {}, 9 }, { 8, HANGAR, {}, 12 }, { 8, TO_ALL, {}, 2 },
	{  9, TO_ALL, AirportBlock::Nothing, 10 },
	/* landing */
	{ 10, TERMGROUP, AirportBlock::Nothing, 10 }, { 10, HELIPAD1, AirportBlock::Helipad1, 14 }, { 10, HANGAR, {}, 1 }, { 10, TO_ALL, {}, 14 },
	/* Helicopter -- takeoff */
	{ 11, HELITAKEOFF, AirportBlock::Nothing, 0 },
	{ 12, TO_ALL, AirportBlock::Hangar2Area, 13 }, // need to go to hangar when waiting in air
	{ 13, TO_ALL, AirportBlock::Hangar2Area, 1 },
	{ 14, HELIPAD1, AirportBlock::Helipad1, 14 }, { 14, HANGAR, {}, 1 }, { 14, HELITAKEOFF, {}, 17 },
	{ 15, HELITAKEOFF, AirportBlock::Nothing, 0 }, // takeoff outside depot
	{ 16, HELITAKEOFF, {}, 14 },
	{ 17, TO_ALL, AirportBlock::Nothing, 11 },
	{ MAX_ELEMENTS, TO_ALL, {}, 0 } // end marker. DO NOT REMOVE
};

/* helistation */
static const HangarTileTable _airport_depots_helistation[] = { {{0, 0}, Direction::SE, 0} };
static const uint8_t _airport_entries_helistation[] = { 25, 25, 25, 25 };
static const AirportFTAbuildup _airport_fta_helistation[] = {
	{  0, HANGAR, AirportBlock::Nothing, 8 },    { 0, HELIPAD1, {}, 1 }, { 0, HELIPAD2, {}, 1 }, { 0, HELIPAD3, {}, 1 }, { 0, HELITAKEOFF, {}, 1 }, { 0, TO_ALL, {}, 0 },
	{  1, TERMGROUP, AirportBlock::Hangar2Area, 0 },  { 1, HANGAR, {}, 0 }, { 1, HELITAKEOFF, {}, 3 }, { 1, TO_ALL, {}, 4 },
	/* landing */
	{  2, FLYING, AirportBlock::Nothing, 28 },   { 2, HELILANDING, {}, 15 }, { 2, TO_ALL, {}, 28 },
	/* helicopter side */
	{  3, HELITAKEOFF, AirportBlock::Nothing, 0 }, // helitakeoff outside hangar2
	{  4, TERMGROUP, AirportBlock::TaxiwayBusy, 0 },  { 4, HANGAR, AirportBlock::Hangar2Area, 1 }, { 4, HELITAKEOFF, {}, 1 }, { 4, TO_ALL, {}, 5 },
	{  5, TERMGROUP, AirportBlock::TaxiwayBusy, 0 },  { 5, HELIPAD1, AirportBlock::Helipad1, 6 }, { 5, HELIPAD2, AirportBlock::Helipad2, 7 }, { 5, HELIPAD3, AirportBlock::Helipad3, 8 }, { 5, TO_ALL, {}, 4 },
	{  6, HELIPAD1, AirportBlock::Helipad1, 5 }, { 6, HANGAR, AirportBlock::Hangar2Area, 5 }, { 6, HELITAKEOFF, {}, 9 }, { 6, TO_ALL, {}, 6 },
	{  7, HELIPAD2, AirportBlock::Helipad2, 5 }, { 7, HANGAR, AirportBlock::Hangar2Area, 5 }, { 7, HELITAKEOFF, {}, 10 }, { 7, TO_ALL, {}, 7 },
	{  8, HELIPAD3, AirportBlock::Helipad3, 5 }, { 8, HANGAR, AirportBlock::Hangar2Area, 5 }, { 8, HELITAKEOFF, {}, 11 }, { 8, TO_ALL, {}, 8 },
	{  9, TO_ALL, AirportBlock::Helipad1, 12 },
	{ 10, TO_ALL, AirportBlock::Helipad2, 13 },
	{ 11, TO_ALL, AirportBlock::Helipad3, 14 },
	{ 12, HELITAKEOFF, AirportBlock::Nothing, 0 },
	{ 13, HELITAKEOFF, AirportBlock::Nothing, 0 },
	{ 14, HELITAKEOFF, AirportBlock::Nothing, 0 },
	/* heli - in flight moves */
	{ 15, HELILANDING, AirportBlock::PreHelipad, 16 },
	{ 16, HELIENDLANDING, AirportBlock::PreHelipad, 16 }, { 16, HELIPAD1, {}, 17 }, { 16, HELIPAD2, {}, 18 }, { 16, HELIPAD3, {}, 19 }, { 16, HANGAR, {}, 23 },
	{ 17, TO_ALL, AirportBlock::Nothing, 20 },
	{ 18, TO_ALL, AirportBlock::Nothing, 21 },
	{ 19, TO_ALL, AirportBlock::Nothing, 22 },
	/* heli landing */
	{ 20, TERMGROUP, AirportBlock::Nothing, 0 }, { 20, HELIPAD1, AirportBlock::Helipad1, 6 }, { 20, HANGAR, {}, 23 }, { 20, TO_ALL, {}, 6 },
	{ 21, TERMGROUP, AirportBlock::Nothing, 0 }, { 21, HELIPAD2, AirportBlock::Helipad2, 7 }, { 21, HANGAR, {}, 23 }, { 21, TO_ALL, {}, 7 },
	{ 22, TERMGROUP, AirportBlock::Nothing, 0 }, { 22, HELIPAD3, AirportBlock::Helipad3, 8 }, { 22, HANGAR, {}, 23 }, { 22, TO_ALL, {}, 8 },
	{ 23, TO_ALL, AirportBlock::Hangar2Area, 24 }, // need to go to helihangar when waiting in air
	{ 24, TO_ALL, AirportBlock::Hangar2Area, 1 },
	{ 25, TO_ALL, AirportBlock::Nothing, 26 },
	{ 26, TO_ALL, AirportBlock::Nothing, 27 },
	{ 27, TO_ALL, AirportBlock::Nothing, 2 },
	{ 28, TO_ALL, AirportBlock::Nothing, 29 },
	{ 29, TO_ALL, AirportBlock::Nothing, 30 },
	{ 30, TO_ALL, AirportBlock::Nothing, 31 },
	{ 31, TO_ALL, AirportBlock::Nothing, 32 },
	{ 32, TO_ALL, AirportBlock::Nothing, 25 },
	{ MAX_ELEMENTS, TO_ALL, {}, 0 } // end marker. DO NOT REMOVE
};

/* ==================== Mega-Flughafen (Fork) ==================== */

/** Fork: die beiden Hangars des Mega-Flughafens. */
static const HangarTileTable _airport_depots_mega[] = { {{11, 2}, Direction::SE, 0}, {{11, 6}, Direction::SE, 1} };


/** Fork: Bewegungspunkte des Mega-Flughafens (99 Stueck). */
static const AirportMovingData _airport_moving_data_mega[99] = {
	{   184,    40, {AirportMovingDataFlag::ExactPosition}, Direction::SE }, // 00 In Hangar 1
	{   184,   104, {AirportMovingDataFlag::ExactPosition}, Direction::SE }, // 01 In Hangar 2
	{    40,    40, {AirportMovingDataFlag::ExactPosition}, Direction::SE }, // 02 Terminal 1
	{    56,    40, {AirportMovingDataFlag::ExactPosition}, Direction::SE }, // 03 Terminal 2
	{    72,    40, {AirportMovingDataFlag::ExactPosition}, Direction::SE }, // 04 Terminal 3
	{    88,    40, {AirportMovingDataFlag::ExactPosition}, Direction::SE }, // 05 Terminal 4
	{    40,    72, {AirportMovingDataFlag::ExactPosition}, Direction::SE }, // 06 Terminal 5
	{    56,    72, {AirportMovingDataFlag::ExactPosition}, Direction::SE }, // 07 Terminal 6
	{    72,    72, {AirportMovingDataFlag::ExactPosition}, Direction::SE }, // 08 Terminal 7
	{    88,    72, {AirportMovingDataFlag::ExactPosition}, Direction::SE }, // 09 Terminal 8
	{   120,    40, {AirportMovingDataFlag::ExactPosition}, Direction::SE }, // 10 Terminal 9
	{   136,    40, {AirportMovingDataFlag::ExactPosition}, Direction::SE }, // 11 Terminal 10
	{   152,    40, {AirportMovingDataFlag::ExactPosition}, Direction::SE }, // 12 Terminal 11
	{   168,    40, {AirportMovingDataFlag::ExactPosition}, Direction::SE }, // 13 Terminal 12
	{   120,    72, {AirportMovingDataFlag::ExactPosition}, Direction::SE }, // 14 Terminal 13
	{   136,    72, {AirportMovingDataFlag::ExactPosition}, Direction::SE }, // 15 Terminal 14
	{   152,    72, {AirportMovingDataFlag::ExactPosition}, Direction::SE }, // 16 Terminal 15
	{   168,    72, {AirportMovingDataFlag::ExactPosition}, Direction::SE }, // 17 Terminal 16
	{    40,   104, {AirportMovingDataFlag::ExactPosition}, Direction::SE }, // 18 Helipad 1
	{    56,   104, {AirportMovingDataFlag::ExactPosition}, Direction::SE }, // 19 Helipad 2
	{     8,    56, {}, Direction::N }, // 20 Rollweg 0/3
	{    24,    56, {}, Direction::N }, // 21 Rollweg 1/3
	{    40,    56, {}, Direction::N }, // 22 Rollweg 2/3
	{    56,    56, {}, Direction::N }, // 23 Rollweg 3/3
	{    72,    56, {}, Direction::N }, // 24 Rollweg 4/3
	{    88,    56, {}, Direction::N }, // 25 Rollweg 5/3
	{   104,    56, {}, Direction::N }, // 26 Rollweg 6/3
	{   120,    56, {}, Direction::N }, // 27 Rollweg 7/3
	{   136,    56, {}, Direction::N }, // 28 Rollweg 8/3
	{   152,    56, {}, Direction::N }, // 29 Rollweg 9/3
	{   168,    56, {}, Direction::N }, // 30 Rollweg 10/3
	{   184,    56, {}, Direction::N }, // 31 Rollweg 11/3
	{     8,    88, {}, Direction::N }, // 32 Rollweg 0/5
	{    24,    88, {}, Direction::N }, // 33 Rollweg 1/5
	{    40,    88, {}, Direction::N }, // 34 Rollweg 2/5
	{    56,    88, {}, Direction::N }, // 35 Rollweg 3/5
	{    72,    88, {}, Direction::N }, // 36 Rollweg 4/5
	{    88,    88, {}, Direction::N }, // 37 Rollweg 5/5
	{   104,    88, {}, Direction::N }, // 38 Rollweg 6/5
	{   120,    88, {}, Direction::N }, // 39 Rollweg 7/5
	{   136,    88, {}, Direction::N }, // 40 Rollweg 8/5
	{   152,    88, {}, Direction::N }, // 41 Rollweg 9/5
	{   168,    88, {}, Direction::N }, // 42 Rollweg 10/5
	{   184,    88, {}, Direction::N }, // 43 Rollweg 11/5
	{     8,   120, {}, Direction::N }, // 44 Rollweg 0/7
	{    24,   120, {}, Direction::N }, // 45 Rollweg 1/7
	{    40,   120, {}, Direction::N }, // 46 Rollweg 2/7
	{    56,   120, {}, Direction::N }, // 47 Rollweg 3/7
	{    72,   120, {}, Direction::N }, // 48 Rollweg 4/7
	{    88,   120, {}, Direction::N }, // 49 Rollweg 5/7
	{   104,   120, {}, Direction::N }, // 50 Rollweg 6/7
	{   120,   120, {}, Direction::N }, // 51 Rollweg 7/7
	{   136,   120, {}, Direction::N }, // 52 Rollweg 8/7
	{   152,   120, {}, Direction::N }, // 53 Rollweg 9/7
	{   168,   120, {}, Direction::N }, // 54 Rollweg 10/7
	{   184,   120, {}, Direction::N }, // 55 Rollweg 11/7
	{    24,    40, {}, Direction::N }, // 56 Rollweg 1/2
	{   104,    40, {}, Direction::N }, // 57 Rollweg 6/2
	{   184,    40, {}, Direction::N }, // 58 Rollweg 11/2
	{    24,    72, {}, Direction::N }, // 59 Rollweg 1/4
	{   104,    72, {}, Direction::N }, // 60 Rollweg 6/4
	{   184,    72, {}, Direction::N }, // 61 Rollweg 11/4
	{    24,   104, {}, Direction::N }, // 62 Rollweg 1/6
	{   104,   104, {}, Direction::N }, // 63 Rollweg 6/6
	{   184,   104, {}, Direction::N }, // 64 Rollweg 11/6
	{     8,   168, {}, Direction::N }, // 65 Rollweg 0/10
	{    24,   168, {}, Direction::N }, // 66 Rollweg 1/10
	{    40,   168, {}, Direction::N }, // 67 Rollweg 2/10
	{    56,   168, {}, Direction::N }, // 68 Rollweg 3/10
	{    72,   168, {}, Direction::N }, // 69 Rollweg 4/10
	{    88,   168, {}, Direction::N }, // 70 Rollweg 5/10
	{   104,   168, {}, Direction::N }, // 71 Rollweg 6/10
	{   120,   168, {}, Direction::N }, // 72 Rollweg 7/10
	{   136,   168, {}, Direction::N }, // 73 Rollweg 8/10
	{   152,   168, {}, Direction::N }, // 74 Rollweg 9/10
	{   168,   168, {}, Direction::N }, // 75 Rollweg 10/10
	{   184,   168, {}, Direction::N }, // 76 Rollweg 11/10
	{     8,    40, {AirportMovingDataFlag::ExactPosition}, Direction::NW }, // 77 Flughafeneinfahrt
	{    24,   120, {}, Direction::N }, // 78 Weg zur Startbahn
	{     8,   136, {AirportMovingDataFlag::ExactPosition}, Direction::SW }, // 79 Beschleunigen auf der Startbahn
	{   152,   136, {AirportMovingDataFlag::NoSpeedClamp}, Direction::N }, // 80 Startbahn freigeben
	{   184,   136, {AirportMovingDataFlag::NoSpeedClamp}, Direction::N }, // 81 Ende der Startbahn
	{   320,   136, {AirportMovingDataFlag::NoSpeedClamp, AirportMovingDataFlag::Takeoff}, Direction::N }, // 82 Abheben
	{   400,     8, {AirportMovingDataFlag::NoSpeedClamp, AirportMovingDataFlag::SlowTurn}, Direction::N }, // 83 Anflugpunkt
	{   184,     8, {AirportMovingDataFlag::NoSpeedClamp, AirportMovingDataFlag::Land}, Direction::N }, // 84 Landeanflug
	{    40,     8, {AirportMovingDataFlag::NoSpeedClamp, AirportMovingDataFlag::Brake}, Direction::N }, // 85 Ausrollen
	{     8,     8, {}, Direction::N }, // 86 Wenden nach der Landung
	{     8,    24, {}, Direction::N }, // 87 Rollen von der Bahn
	{   104,   404, {AirportMovingDataFlag::NoSpeedClamp, AirportMovingDataFlag::SlowTurn}, Direction::N }, // 88 Warteschleife Sued
	{   420,    88, {AirportMovingDataFlag::NoSpeedClamp, AirportMovingDataFlag::SlowTurn}, Direction::N }, // 89 Warteschleife Ost
	{   104,  -220, {AirportMovingDataFlag::NoSpeedClamp, AirportMovingDataFlag::SlowTurn}, Direction::N }, // 90 Warteschleife Nord
	{  -260,    88, {AirportMovingDataFlag::NoSpeedClamp, AirportMovingDataFlag::SlowTurn}, Direction::N }, // 91 Warteschleife West
	{    40,   120, {AirportMovingDataFlag::NoSpeedClamp}, Direction::N }, // 92 Warteraum vor den Helipads
	{    40,   104, {AirportMovingDataFlag::NoSpeedClamp, AirportMovingDataFlag::SlowTurn}, Direction::N }, // 93 Anflug Helipad 1
	{    56,   104, {AirportMovingDataFlag::NoSpeedClamp, AirportMovingDataFlag::SlowTurn}, Direction::N }, // 94 Anflug Helipad 2
	{    40,   104, {AirportMovingDataFlag::HeliLower}, Direction::N }, // 95 Landen Helipad 1
	{    56,   104, {AirportMovingDataFlag::HeliLower}, Direction::N }, // 96 Landen Helipad 2
	{    40,   104, {AirportMovingDataFlag::HeliRaise}, Direction::N }, // 97 Abheben Helipad 1
	{    56,   104, {AirportMovingDataFlag::HeliRaise}, Direction::N }, // 98 Abheben Helipad 2
};



/**
 * Fork: Flugsteuerung des Mega-Flughafens.
 *
 * Die Bodenwege sind nicht von Hand geschrieben, sondern aus einem
 * Graphen berechnet: fuer jedes Ziel liefert eine Breitensuche den
 * naechsten Schritt. Dadurch findet jedes Flugzeug von jeder Position
 * aus jedes Terminal, ohne dass eine Kombination vergessen werden kann.
 */
static const AirportFTAbuildup _airport_fta_mega[] = {
	{   0, HANGAR, AirportBlock::Nothing, 0 }, {   0, TO_ALL, AirportBlock::Nothing, 58 },  // In Hangar 1
	{   1, HANGAR, AirportBlock::Nothing, 1 }, {   1, TO_ALL, AirportBlock::Nothing, 64 },  // In Hangar 2
	{   2, TERM1, AirportBlock::Term1, 2 }, {   2, TO_ALL, AirportBlock::Nothing, 22 },  // Terminal 1
	{   3, TERM2, AirportBlock::Term2, 3 }, {   3, TO_ALL, AirportBlock::Nothing, 23 },  // Terminal 2
	{   4, TERM3, AirportBlock::Term3, 4 }, {   4, TO_ALL, AirportBlock::Nothing, 24 },  // Terminal 3
	{   5, TERM4, AirportBlock::Term4, 5 }, {   5, TO_ALL, AirportBlock::Nothing, 25 },  // Terminal 4
	{   6, HANGAR, AirportBlock::Nothing, 22 }, {   6, TERM1, AirportBlock::Nothing, 22 }, {   6, TERM2, AirportBlock::Nothing, 22 }, {   6, TERM3, AirportBlock::Nothing, 22 }, {   6, TERM4, AirportBlock::Nothing, 22 }, {   6, TERM5, AirportBlock::Term5, 6 }, {   6, TERM7, AirportBlock::Nothing, 22 }, {   6, TERM8, AirportBlock::Nothing, 22 }, {   6, TERM9, AirportBlock::Nothing, 22 }, {   6, TERM10, AirportBlock::Nothing, 22 }, {   6, TERM11, AirportBlock::Nothing, 22 }, {   6, TERM12, AirportBlock::Nothing, 22 }, {   6, TERM13, AirportBlock::Nothing, 22 }, {   6, TO_ALL, AirportBlock::Nothing, 34 },  // Terminal 5
	{   7, HANGAR, AirportBlock::Nothing, 23 }, {   7, TERM1, AirportBlock::Nothing, 23 }, {   7, TERM2, AirportBlock::Nothing, 23 }, {   7, TERM3, AirportBlock::Nothing, 23 }, {   7, TERM4, AirportBlock::Nothing, 23 }, {   7, TERM6, AirportBlock::Term6, 7 }, {   7, TERM7, AirportBlock::Nothing, 23 }, {   7, TERM8, AirportBlock::Nothing, 23 }, {   7, TERM9, AirportBlock::Nothing, 23 }, {   7, TERM10, AirportBlock::Nothing, 23 }, {   7, TERM11, AirportBlock::Nothing, 23 }, {   7, TERM12, AirportBlock::Nothing, 23 }, {   7, TERM13, AirportBlock::Nothing, 23 }, {   7, TO_ALL, AirportBlock::Nothing, 35 },  // Terminal 6
	{   8, HANGAR, AirportBlock::Nothing, 24 }, {   8, TERM1, AirportBlock::Nothing, 24 }, {   8, TERM2, AirportBlock::Nothing, 24 }, {   8, TERM3, AirportBlock::Nothing, 24 }, {   8, TERM4, AirportBlock::Nothing, 24 }, {   8, TERM7, AirportBlock::Term7, 8 }, {   8, TERM8, AirportBlock::Nothing, 24 }, {   8, TERM9, AirportBlock::Nothing, 24 }, {   8, TERM10, AirportBlock::Nothing, 24 }, {   8, TERM11, AirportBlock::Nothing, 24 }, {   8, TERM12, AirportBlock::Nothing, 24 }, {   8, TERM13, AirportBlock::Nothing, 24 }, {   8, TO_ALL, AirportBlock::Nothing, 36 },  // Terminal 7
	{   9, HANGAR, AirportBlock::Nothing, 25 }, {   9, TERM1, AirportBlock::Nothing, 25 }, {   9, TERM2, AirportBlock::Nothing, 25 }, {   9, TERM3, AirportBlock::Nothing, 25 }, {   9, TERM4, AirportBlock::Nothing, 25 }, {   9, TERM7, AirportBlock::Nothing, 25 }, {   9, TERM8, AirportBlock::Term8, 9 }, {   9, TERM9, AirportBlock::Nothing, 25 }, {   9, TERM10, AirportBlock::Nothing, 25 }, {   9, TERM11, AirportBlock::Nothing, 25 }, {   9, TERM12, AirportBlock::Nothing, 25 }, {   9, TERM13, AirportBlock::Nothing, 25 }, {   9, TO_ALL, AirportBlock::Nothing, 37 },  // Terminal 8
	{  10, TERM9, AirportBlock::Term9, 10 }, {  10, TO_ALL, AirportBlock::Nothing, 27 },  // Terminal 9
	{  11, TERM10, AirportBlock::Term10, 11 }, {  11, TO_ALL, AirportBlock::Nothing, 28 },  // Terminal 10
	{  12, TERM11, AirportBlock::Term11, 12 }, {  12, TO_ALL, AirportBlock::Nothing, 29 },  // Terminal 11
	{  13, TERM12, AirportBlock::Term12, 13 }, {  13, TO_ALL, AirportBlock::Nothing, 30 },  // Terminal 12
	{  14, HANGAR, AirportBlock::Nothing, 27 }, {  14, TERM1, AirportBlock::Nothing, 27 }, {  14, TERM2, AirportBlock::Nothing, 27 }, {  14, TERM3, AirportBlock::Nothing, 27 }, {  14, TERM4, AirportBlock::Nothing, 27 }, {  14, TERM7, AirportBlock::Nothing, 27 }, {  14, TERM8, AirportBlock::Nothing, 27 }, {  14, TERM9, AirportBlock::Nothing, 27 }, {  14, TERM10, AirportBlock::Nothing, 27 }, {  14, TERM11, AirportBlock::Nothing, 27 }, {  14, TERM12, AirportBlock::Nothing, 27 }, {  14, TERM13, AirportBlock::Term13, 14 }, {  14, TO_ALL, AirportBlock::Nothing, 39 },  // Terminal 13
	{  15, HANGAR, AirportBlock::Nothing, 28 }, {  15, TERM1, AirportBlock::Nothing, 28 }, {  15, TERM2, AirportBlock::Nothing, 28 }, {  15, TERM3, AirportBlock::Nothing, 28 }, {  15, TERM4, AirportBlock::Nothing, 28 }, {  15, TERM7, AirportBlock::Nothing, 28 }, {  15, TERM8, AirportBlock::Nothing, 28 }, {  15, TERM9, AirportBlock::Nothing, 28 }, {  15, TERM10, AirportBlock::Nothing, 28 }, {  15, TERM11, AirportBlock::Nothing, 28 }, {  15, TERM12, AirportBlock::Nothing, 28 }, {  15, TERM13, AirportBlock::Nothing, 28 }, {  15, TERM14, AirportBlock::Term14, 15 }, {  15, TO_ALL, AirportBlock::Nothing, 40 },  // Terminal 14
	{  16, HANGAR, AirportBlock::Nothing, 29 }, {  16, TERM1, AirportBlock::Nothing, 29 }, {  16, TERM2, AirportBlock::Nothing, 29 }, {  16, TERM3, AirportBlock::Nothing, 29 }, {  16, TERM4, AirportBlock::Nothing, 29 }, {  16, TERM7, AirportBlock::Nothing, 29 }, {  16, TERM8, AirportBlock::Nothing, 29 }, {  16, TERM9, AirportBlock::Nothing, 29 }, {  16, TERM10, AirportBlock::Nothing, 29 }, {  16, TERM11, AirportBlock::Nothing, 29 }, {  16, TERM12, AirportBlock::Nothing, 29 }, {  16, TERM13, AirportBlock::Nothing, 29 }, {  16, TERM15, AirportBlock::Term15, 16 }, {  16, TO_ALL, AirportBlock::Nothing, 41 },  // Terminal 15
	{  17, HANGAR, AirportBlock::Nothing, 30 }, {  17, TERM1, AirportBlock::Nothing, 30 }, {  17, TERM2, AirportBlock::Nothing, 30 }, {  17, TERM3, AirportBlock::Nothing, 30 }, {  17, TERM4, AirportBlock::Nothing, 30 }, {  17, TERM7, AirportBlock::Nothing, 30 }, {  17, TERM8, AirportBlock::Nothing, 30 }, {  17, TERM9, AirportBlock::Nothing, 30 }, {  17, TERM10, AirportBlock::Nothing, 30 }, {  17, TERM11, AirportBlock::Nothing, 30 }, {  17, TERM12, AirportBlock::Nothing, 30 }, {  17, TERM13, AirportBlock::Nothing, 30 }, {  17, TERM16, AirportBlock::Term16, 17 }, {  17, TO_ALL, AirportBlock::Nothing, 42 },  // Terminal 16
	{  18, HANGAR, AirportBlock::Nothing, 34 }, {  18, TERM1, AirportBlock::Nothing, 34 }, {  18, TERM2, AirportBlock::Nothing, 34 }, {  18, TERM3, AirportBlock::Nothing, 34 }, {  18, TERM4, AirportBlock::Nothing, 34 }, {  18, TERM5, AirportBlock::Nothing, 34 }, {  18, TERM6, AirportBlock::Nothing, 34 }, {  18, TERM7, AirportBlock::Nothing, 34 }, {  18, TERM8, AirportBlock::Nothing, 34 }, {  18, TERM9, AirportBlock::Nothing, 34 }, {  18, TERM10, AirportBlock::Nothing, 34 }, {  18, TERM11, AirportBlock::Nothing, 34 }, {  18, TERM12, AirportBlock::Nothing, 34 }, {  18, TERM13, AirportBlock::Nothing, 34 }, {  18, TERM14, AirportBlock::Nothing, 34 }, {  18, TERM15, AirportBlock::Nothing, 34 }, {  18, TERM16, AirportBlock::Nothing, 34 }, {  18, HELIPAD1, AirportBlock::Helipad1, 18 }, {  18, HELIPAD2, AirportBlock::Nothing, 34 }, {  18, HELITAKEOFF, AirportBlock::Nothing, 97 }, {  18, TO_ALL, AirportBlock::Nothing, 46 },  // Helipad 1
	{  19, HANGAR, AirportBlock::Nothing, 35 }, {  19, TERM1, AirportBlock::Nothing, 35 }, {  19, TERM2, AirportBlock::Nothing, 35 }, {  19, TERM3, AirportBlock::Nothing, 35 }, {  19, TERM4, AirportBlock::Nothing, 35 }, {  19, TERM5, AirportBlock::Nothing, 35 }, {  19, TERM6, AirportBlock::Nothing, 35 }, {  19, TERM7, AirportBlock::Nothing, 35 }, {  19, TERM8, AirportBlock::Nothing, 35 }, {  19, TERM9, AirportBlock::Nothing, 35 }, {  19, TERM10, AirportBlock::Nothing, 35 }, {  19, TERM11, AirportBlock::Nothing, 35 }, {  19, TERM12, AirportBlock::Nothing, 35 }, {  19, TERM13, AirportBlock::Nothing, 35 }, {  19, TERM14, AirportBlock::Nothing, 35 }, {  19, TERM15, AirportBlock::Nothing, 35 }, {  19, TERM16, AirportBlock::Nothing, 35 }, {  19, HELIPAD1, AirportBlock::Nothing, 35 }, {  19, HELIPAD2, AirportBlock::Helipad2, 19 }, {  19, HELITAKEOFF, AirportBlock::Nothing, 98 }, {  19, TO_ALL, AirportBlock::Nothing, 47 },  // Helipad 2
	{  20, TERMGROUP, AirportBlock::Nothing, 20 }, {  20, TO_ALL, AirportBlock::Nothing, 21 },  // Rollweg 0/3
	{  21, TERMGROUP, AirportBlock::Nothing, 21 }, {  21, HANGAR, AirportBlock::Nothing, 22 }, {  21, TERM1, AirportBlock::Nothing, 22 }, {  21, TERM2, AirportBlock::Nothing, 22 }, {  21, TERM3, AirportBlock::Nothing, 22 }, {  21, TERM4, AirportBlock::Nothing, 22 }, {  21, TERM5, AirportBlock::Nothing, 22 }, {  21, TERM6, AirportBlock::Nothing, 22 }, {  21, TERM7, AirportBlock::Nothing, 22 }, {  21, TERM8, AirportBlock::Nothing, 22 }, {  21, TERM9, AirportBlock::Nothing, 22 }, {  21, TERM10, AirportBlock::Nothing, 22 }, {  21, TERM11, AirportBlock::Nothing, 22 }, {  21, TERM12, AirportBlock::Nothing, 22 }, {  21, TERM13, AirportBlock::Nothing, 22 }, {  21, TERM14, AirportBlock::Nothing, 22 }, {  21, TERM15, AirportBlock::Nothing, 22 }, {  21, TERM16, AirportBlock::Nothing, 22 }, {  21, TO_ALL, AirportBlock::Nothing, 59 },  // Rollweg 1/3
	{  22, TERMGROUP, AirportBlock::Nothing, 22 }, {  22, HANGAR, AirportBlock::Nothing, 23 }, {  22, TERM1, AirportBlock::Term1, 2 }, {  22, TERM2, AirportBlock::Nothing, 23 }, {  22, TERM3, AirportBlock::Nothing, 23 }, {  22, TERM4, AirportBlock::Nothing, 23 }, {  22, TERM6, AirportBlock::Nothing, 23 }, {  22, TERM7, AirportBlock::Nothing, 23 }, {  22, TERM8, AirportBlock::Nothing, 23 }, {  22, TERM9, AirportBlock::Nothing, 23 }, {  22, TERM10, AirportBlock::Nothing, 23 }, {  22, TERM11, AirportBlock::Nothing, 23 }, {  22, TERM12, AirportBlock::Nothing, 23 }, {  22, TERM13, AirportBlock::Nothing, 23 }, {  22, TERM14, AirportBlock::Nothing, 23 }, {  22, TERM15, AirportBlock::Nothing, 23 }, {  22, TERM16, AirportBlock::Nothing, 23 }, {  22, TO_ALL, AirportBlock::Term5, 6 },  // Rollweg 2/3
	{  23, TERMGROUP, AirportBlock::Nothing, 23 }, {  23, HANGAR, AirportBlock::Nothing, 24 }, {  23, TERM1, AirportBlock::Nothing, 22 }, {  23, TERM2, AirportBlock::Term2, 3 }, {  23, TERM3, AirportBlock::Nothing, 24 }, {  23, TERM4, AirportBlock::Nothing, 24 }, {  23, TERM5, AirportBlock::Nothing, 22 }, {  23, TERM7, AirportBlock::Nothing, 24 }, {  23, TERM8, AirportBlock::Nothing, 24 }, {  23, TERM9, AirportBlock::Nothing, 24 }, {  23, TERM10, AirportBlock::Nothing, 24 }, {  23, TERM11, AirportBlock::Nothing, 24 }, {  23, TERM12, AirportBlock::Nothing, 24 }, {  23, TERM13, AirportBlock::Nothing, 24 }, {  23, TERM14, AirportBlock::Nothing, 24 }, {  23, TERM15, AirportBlock::Nothing, 24 }, {  23, TERM16, AirportBlock::Nothing, 24 }, {  23, TO_ALL, AirportBlock::Term6, 7 },  // Rollweg 3/3
	{  24, TERMGROUP, AirportBlock::Nothing, 24 }, {  24, HANGAR, AirportBlock::Nothing, 25 }, {  24, TERM1, AirportBlock::Nothing, 23 }, {  24, TERM2, AirportBlock::Nothing, 23 }, {  24, TERM3, AirportBlock::Term3, 4 }, {  24, TERM4, AirportBlock::Nothing, 25 }, {  24, TERM5, AirportBlock::Nothing, 23 }, {  24, TERM6, AirportBlock::Nothing, 23 }, {  24, TERM8, AirportBlock::Nothing, 25 }, {  24, TERM9, AirportBlock::Nothing, 25 }, {  24, TERM10, AirportBlock::Nothing, 25 }, {  24, TERM11, AirportBlock::Nothing, 25 }, {  24, TERM12, AirportBlock::Nothing, 25 }, {  24, TERM13, AirportBlock::Nothing, 25 }, {  24, TERM14, AirportBlock::Nothing, 25 }, {  24, TERM15, AirportBlock::Nothing, 25 }, {  24, TERM16, AirportBlock::Nothing, 25 }, {  24, TO_ALL, AirportBlock::Term7, 8 },  // Rollweg 4/3
	{  25, TERMGROUP, AirportBlock::Nothing, 25 }, {  25, HANGAR, AirportBlock::Nothing, 26 }, {  25, TERM4, AirportBlock::Term4, 5 }, {  25, TERM8, AirportBlock::Term8, 9 }, {  25, TERM9, AirportBlock::Nothing, 26 }, {  25, TERM10, AirportBlock::Nothing, 26 }, {  25, TERM11, AirportBlock::Nothing, 26 }, {  25, TERM12, AirportBlock::Nothing, 26 }, {  25, TERM13, AirportBlock::Nothing, 26 }, {  25, TERM14, AirportBlock::Nothing, 26 }, {  25, TERM15, AirportBlock::Nothing, 26 }, {  25, TERM16, AirportBlock::Nothing, 26 }, {  25, TO_ALL, AirportBlock::Nothing, 24 },  // Rollweg 5/3
	{  26, TERMGROUP, AirportBlock::Nothing, 26 }, {  26, HANGAR, AirportBlock::Nothing, 27 }, {  26, TERM9, AirportBlock::Nothing, 27 }, {  26, TERM10, AirportBlock::Nothing, 27 }, {  26, TERM11, AirportBlock::Nothing, 27 }, {  26, TERM12, AirportBlock::Nothing, 27 }, {  26, TERM13, AirportBlock::Nothing, 27 }, {  26, TERM14, AirportBlock::Nothing, 27 }, {  26, TERM15, AirportBlock::Nothing, 27 }, {  26, TERM16, AirportBlock::Nothing, 27 }, {  26, TO_ALL, AirportBlock::Nothing, 25 },  // Rollweg 6/3
	{  27, TERMGROUP, AirportBlock::Nothing, 27 }, {  27, HANGAR, AirportBlock::Nothing, 28 }, {  27, TERM9, AirportBlock::Term9, 10 }, {  27, TERM10, AirportBlock::Nothing, 28 }, {  27, TERM11, AirportBlock::Nothing, 28 }, {  27, TERM12, AirportBlock::Nothing, 28 }, {  27, TERM13, AirportBlock::Term13, 14 }, {  27, TERM14, AirportBlock::Nothing, 28 }, {  27, TERM15, AirportBlock::Nothing, 28 }, {  27, TERM16, AirportBlock::Nothing, 28 }, {  27, TO_ALL, AirportBlock::Nothing, 26 },  // Rollweg 7/3
	{  28, TERMGROUP, AirportBlock::Nothing, 28 }, {  28, HANGAR, AirportBlock::Nothing, 29 }, {  28, TERM10, AirportBlock::Term10, 11 }, {  28, TERM11, AirportBlock::Nothing, 29 }, {  28, TERM12, AirportBlock::Nothing, 29 }, {  28, TERM14, AirportBlock::Term14, 15 }, {  28, TERM15, AirportBlock::Nothing, 29 }, {  28, TERM16, AirportBlock::Nothing, 29 }, {  28, TO_ALL, AirportBlock::Nothing, 27 },  // Rollweg 8/3
	{  29, TERMGROUP, AirportBlock::Nothing, 29 }, {  29, HANGAR, AirportBlock::Nothing, 30 }, {  29, TERM11, AirportBlock::Term11, 12 }, {  29, TERM12, AirportBlock::Nothing, 30 }, {  29, TERM15, AirportBlock::Term15, 16 }, {  29, TERM16, AirportBlock::Nothing, 30 }, {  29, TO_ALL, AirportBlock::Nothing, 28 },  // Rollweg 9/3
	{  30, TERMGROUP, AirportBlock::Nothing, 30 }, {  30, HANGAR, AirportBlock::Nothing, 31 }, {  30, TERM12, AirportBlock::Term12, 13 }, {  30, TERM16, AirportBlock::Term16, 17 }, {  30, TO_ALL, AirportBlock::Nothing, 29 },  // Rollweg 10/3
	{  31, TERMGROUP, AirportBlock::Nothing, 31 }, {  31, HANGAR, AirportBlock::Nothing, 58 }, {  31, TO_ALL, AirportBlock::Nothing, 30 },  // Rollweg 11/3
	{  32, TERMGROUP, AirportBlock::Nothing, 32 }, {  32, TO_ALL, AirportBlock::Nothing, 33 },  // Rollweg 0/5
	{  33, TERMGROUP, AirportBlock::Nothing, 33 }, {  33, HANGAR, AirportBlock::Nothing, 34 }, {  33, TERM1, AirportBlock::Nothing, 59 }, {  33, TERM2, AirportBlock::Nothing, 59 }, {  33, TERM3, AirportBlock::Nothing, 34 }, {  33, TERM4, AirportBlock::Nothing, 34 }, {  33, TERM5, AirportBlock::Nothing, 34 }, {  33, TERM6, AirportBlock::Nothing, 34 }, {  33, TERM7, AirportBlock::Nothing, 34 }, {  33, TERM8, AirportBlock::Nothing, 34 }, {  33, TERM9, AirportBlock::Nothing, 34 }, {  33, TERM10, AirportBlock::Nothing, 34 }, {  33, TERM11, AirportBlock::Nothing, 34 }, {  33, TERM12, AirportBlock::Nothing, 34 }, {  33, TERM13, AirportBlock::Nothing, 34 }, {  33, TERM14, AirportBlock::Nothing, 34 }, {  33, TERM15, AirportBlock::Nothing, 34 }, {  33, TERM16, AirportBlock::Nothing, 34 }, {  33, HELIPAD1, AirportBlock::Nothing, 34 }, {  33, HELIPAD2, AirportBlock::Nothing, 34 }, {  33, HELITAKEOFF, AirportBlock::Nothing, 34 }, {  33, TO_ALL, AirportBlock::Nothing, 62 },  // Rollweg 1/5
	{  34, TERMGROUP, AirportBlock::Nothing, 34 }, {  34, HANGAR, AirportBlock::Nothing, 35 }, {  34, TERM1, AirportBlock::Term5, 6 }, {  34, TERM2, AirportBlock::Term5, 6 }, {  34, TERM3, AirportBlock::Nothing, 35 }, {  34, TERM4, AirportBlock::Nothing, 35 }, {  34, TERM5, AirportBlock::Term5, 6 }, {  34, TERM6, AirportBlock::Nothing, 35 }, {  34, TERM7, AirportBlock::Nothing, 35 }, {  34, TERM8, AirportBlock::Nothing, 35 }, {  34, TERM9, AirportBlock::Nothing, 35 }, {  34, TERM10, AirportBlock::Nothing, 35 }, {  34, TERM11, AirportBlock::Nothing, 35 }, {  34, TERM12, AirportBlock::Nothing, 35 }, {  34, TERM13, AirportBlock::Nothing, 35 }, {  34, TERM14, AirportBlock::Nothing, 35 }, {  34, TERM15, AirportBlock::Nothing, 35 }, {  34, TERM16, AirportBlock::Nothing, 35 }, {  34, HELIPAD2, AirportBlock::Nothing, 35 }, {  34, TO_ALL, AirportBlock::Helipad1, 18 },  // Rollweg 2/5
	{  35, TERMGROUP, AirportBlock::Nothing, 35 }, {  35, HANGAR, AirportBlock::Nothing, 36 }, {  35, TERM2, AirportBlock::Term6, 7 }, {  35, TERM3, AirportBlock::Nothing, 36 }, {  35, TERM4, AirportBlock::Nothing, 36 }, {  35, TERM6, AirportBlock::Term6, 7 }, {  35, TERM7, AirportBlock::Nothing, 36 }, {  35, TERM8, AirportBlock::Nothing, 36 }, {  35, TERM9, AirportBlock::Nothing, 36 }, {  35, TERM10, AirportBlock::Nothing, 36 }, {  35, TERM11, AirportBlock::Nothing, 36 }, {  35, TERM12, AirportBlock::Nothing, 36 }, {  35, TERM13, AirportBlock::Nothing, 36 }, {  35, TERM14, AirportBlock::Nothing, 36 }, {  35, TERM15, AirportBlock::Nothing, 36 }, {  35, TERM16, AirportBlock::Nothing, 36 }, {  35, HELIPAD2, AirportBlock::Helipad2, 19 }, {  35, TO_ALL, AirportBlock::Nothing, 34 },  // Rollweg 3/5
	{  36, TERMGROUP, AirportBlock::Nothing, 36 }, {  36, HANGAR, AirportBlock::Nothing, 37 }, {  36, TERM2, AirportBlock::Term7, 8 }, {  36, TERM3, AirportBlock::Term7, 8 }, {  36, TERM4, AirportBlock::Term7, 8 }, {  36, TERM7, AirportBlock::Term7, 8 }, {  36, TERM8, AirportBlock::Nothing, 37 }, {  36, TERM9, AirportBlock::Term7, 8 }, {  36, TERM10, AirportBlock::Term7, 8 }, {  36, TERM11, AirportBlock::Nothing, 37 }, {  36, TERM12, AirportBlock::Nothing, 37 }, {  36, TERM13, AirportBlock::Nothing, 37 }, {  36, TERM14, AirportBlock::Nothing, 37 }, {  36, TERM15, AirportBlock::Nothing, 37 }, {  36, TERM16, AirportBlock::Nothing, 37 }, {  36, TO_ALL, AirportBlock::Nothing, 35 },  // Rollweg 4/5
	{  37, TERMGROUP, AirportBlock::Nothing, 37 }, {  37, HANGAR, AirportBlock::Nothing, 38 }, {  37, TERM4, AirportBlock::Term8, 9 }, {  37, TERM8, AirportBlock::Term8, 9 }, {  37, TERM9, AirportBlock::Term8, 9 }, {  37, TERM10, AirportBlock::Term8, 9 }, {  37, TERM11, AirportBlock::Nothing, 38 }, {  37, TERM12, AirportBlock::Nothing, 38 }, {  37, TERM13, AirportBlock::Nothing, 38 }, {  37, TERM14, AirportBlock::Nothing, 38 }, {  37, TERM15, AirportBlock::Nothing, 38 }, {  37, TERM16, AirportBlock::Nothing, 38 }, {  37, TO_ALL, AirportBlock::Nothing, 36 },  // Rollweg 5/5
	{  38, TERMGROUP, AirportBlock::Nothing, 38 }, {  38, HANGAR, AirportBlock::Nothing, 39 }, {  38, TERM9, AirportBlock::Nothing, 60 }, {  38, TERM10, AirportBlock::Nothing, 60 }, {  38, TERM11, AirportBlock::Nothing, 39 }, {  38, TERM12, AirportBlock::Nothing, 39 }, {  38, TERM13, AirportBlock::Nothing, 39 }, {  38, TERM14, AirportBlock::Nothing, 39 }, {  38, TERM15, AirportBlock::Nothing, 39 }, {  38, TERM16, AirportBlock::Nothing, 39 }, {  38, TO_ALL, AirportBlock::Nothing, 37 },  // Rollweg 6/5
	{  39, TERMGROUP, AirportBlock::Nothing, 39 }, {  39, HANGAR, AirportBlock::Nothing, 40 }, {  39, TERM9, AirportBlock::Term13, 14 }, {  39, TERM10, AirportBlock::Term13, 14 }, {  39, TERM11, AirportBlock::Nothing, 40 }, {  39, TERM12, AirportBlock::Nothing, 40 }, {  39, TERM13, AirportBlock::Term13, 14 }, {  39, TERM14, AirportBlock::Nothing, 40 }, {  39, TERM15, AirportBlock::Nothing, 40 }, {  39, TERM16, AirportBlock::Nothing, 40 }, {  39, TO_ALL, AirportBlock::Nothing, 38 },  // Rollweg 7/5
	{  40, TERMGROUP, AirportBlock::Nothing, 40 }, {  40, HANGAR, AirportBlock::Nothing, 41 }, {  40, TERM9, AirportBlock::Term14, 15 }, {  40, TERM10, AirportBlock::Term14, 15 }, {  40, TERM11, AirportBlock::Nothing, 41 }, {  40, TERM12, AirportBlock::Nothing, 41 }, {  40, TERM14, AirportBlock::Term14, 15 }, {  40, TERM15, AirportBlock::Nothing, 41 }, {  40, TERM16, AirportBlock::Nothing, 41 }, {  40, TO_ALL, AirportBlock::Nothing, 39 },  // Rollweg 8/5
	{  41, TERMGROUP, AirportBlock::Nothing, 41 }, {  41, HANGAR, AirportBlock::Nothing, 42 }, {  41, TERM9, AirportBlock::Term15, 16 }, {  41, TERM10, AirportBlock::Term15, 16 }, {  41, TERM11, AirportBlock::Term15, 16 }, {  41, TERM12, AirportBlock::Nothing, 42 }, {  41, TERM15, AirportBlock::Term15, 16 }, {  41, TERM16, AirportBlock::Nothing, 42 }, {  41, TO_ALL, AirportBlock::Nothing, 40 },  // Rollweg 9/5
	{  42, TERMGROUP, AirportBlock::Nothing, 42 }, {  42, HANGAR, AirportBlock::Nothing, 43 }, {  42, TERM12, AirportBlock::Term16, 17 }, {  42, TERM16, AirportBlock::Term16, 17 }, {  42, TO_ALL, AirportBlock::Nothing, 41 },  // Rollweg 10/5
	{  43, TERMGROUP, AirportBlock::Nothing, 43 }, {  43, HANGAR, AirportBlock::Nothing, 61 }, {  43, TO_ALL, AirportBlock::Nothing, 42 },  // Rollweg 11/5
	{  44, TERMGROUP, AirportBlock::Nothing, 44 }, {  44, TO_ALL, AirportBlock::Nothing, 45 },  // Rollweg 0/7
	{  45, TERMGROUP, AirportBlock::Nothing, 45 }, {  45, HANGAR, AirportBlock::Nothing, 46 }, {  45, TERM1, AirportBlock::Nothing, 62 }, {  45, TERM2, AirportBlock::Nothing, 62 }, {  45, TERM3, AirportBlock::Nothing, 62 }, {  45, TERM4, AirportBlock::Nothing, 62 }, {  45, TERM5, AirportBlock::Nothing, 62 }, {  45, TERM6, AirportBlock::Nothing, 62 }, {  45, TERM7, AirportBlock::Nothing, 62 }, {  45, TERM8, AirportBlock::Nothing, 62 }, {  45, TERM9, AirportBlock::Nothing, 62 }, {  45, TERM10, AirportBlock::Nothing, 62 }, {  45, TERM11, AirportBlock::Nothing, 46 }, {  45, TERM12, AirportBlock::Nothing, 46 }, {  45, TERM13, AirportBlock::Nothing, 46 }, {  45, TERM14, AirportBlock::Nothing, 46 }, {  45, TERM15, AirportBlock::Nothing, 46 }, {  45, TERM16, AirportBlock::Nothing, 46 }, {  45, HELIPAD1, AirportBlock::Nothing, 46 }, {  45, HELIPAD2, AirportBlock::Nothing, 46 }, {  45, HELITAKEOFF, AirportBlock::Nothing, 46 }, {  45, TO_ALL, AirportBlock::OutWay, 78 },  // Rollweg 1/7
	{  46, TERMGROUP, AirportBlock::Nothing, 46 }, {  46, HANGAR, AirportBlock::Nothing, 47 }, {  46, TERM1, AirportBlock::Helipad1, 18 }, {  46, TERM2, AirportBlock::Helipad1, 18 }, {  46, TERM3, AirportBlock::Helipad1, 18 }, {  46, TERM4, AirportBlock::Helipad1, 18 }, {  46, TERM5, AirportBlock::Helipad1, 18 }, {  46, TERM6, AirportBlock::Helipad1, 18 }, {  46, TERM7, AirportBlock::Helipad1, 18 }, {  46, TERM8, AirportBlock::Helipad1, 18 }, {  46, TERM9, AirportBlock::Helipad1, 18 }, {  46, TERM10, AirportBlock::Helipad1, 18 }, {  46, TERM11, AirportBlock::Nothing, 47 }, {  46, TERM12, AirportBlock::Nothing, 47 }, {  46, TERM13, AirportBlock::Nothing, 47 }, {  46, TERM14, AirportBlock::Nothing, 47 }, {  46, TERM15, AirportBlock::Nothing, 47 }, {  46, TERM16, AirportBlock::Nothing, 47 }, {  46, HELIPAD1, AirportBlock::Helipad1, 18 }, {  46, HELIPAD2, AirportBlock::Nothing, 47 }, {  46, HELITAKEOFF, AirportBlock::Helipad1, 18 }, {  46, TO_ALL, AirportBlock::Nothing, 45 },  // Rollweg 2/7
	{  47, TERMGROUP, AirportBlock::Nothing, 47 }, {  47, HANGAR, AirportBlock::Nothing, 48 }, {  47, TERM2, AirportBlock::Helipad2, 19 }, {  47, TERM3, AirportBlock::Helipad2, 19 }, {  47, TERM4, AirportBlock::Helipad2, 19 }, {  47, TERM6, AirportBlock::Helipad2, 19 }, {  47, TERM7, AirportBlock::Helipad2, 19 }, {  47, TERM8, AirportBlock::Helipad2, 19 }, {  47, TERM9, AirportBlock::Helipad2, 19 }, {  47, TERM10, AirportBlock::Helipad2, 19 }, {  47, TERM11, AirportBlock::Nothing, 48 }, {  47, TERM12, AirportBlock::Nothing, 48 }, {  47, TERM13, AirportBlock::Nothing, 48 }, {  47, TERM14, AirportBlock::Nothing, 48 }, {  47, TERM15, AirportBlock::Nothing, 48 }, {  47, TERM16, AirportBlock::Nothing, 48 }, {  47, HELIPAD2, AirportBlock::Helipad2, 19 }, {  47, TO_ALL, AirportBlock::Nothing, 46 },  // Rollweg 3/7
	{  48, TERMGROUP, AirportBlock::Nothing, 48 }, {  48, HANGAR, AirportBlock::Nothing, 49 }, {  48, TERM9, AirportBlock::Nothing, 49 }, {  48, TERM10, AirportBlock::Nothing, 49 }, {  48, TERM11, AirportBlock::Nothing, 49 }, {  48, TERM12, AirportBlock::Nothing, 49 }, {  48, TERM13, AirportBlock::Nothing, 49 }, {  48, TERM14, AirportBlock::Nothing, 49 }, {  48, TERM15, AirportBlock::Nothing, 49 }, {  48, TERM16, AirportBlock::Nothing, 49 }, {  48, TO_ALL, AirportBlock::Nothing, 47 },  // Rollweg 4/7
	{  49, TERMGROUP, AirportBlock::Nothing, 49 }, {  49, HANGAR, AirportBlock::Nothing, 50 }, {  49, TERM4, AirportBlock::Nothing, 50 }, {  49, TERM8, AirportBlock::Nothing, 50 }, {  49, TERM9, AirportBlock::Nothing, 50 }, {  49, TERM10, AirportBlock::Nothing, 50 }, {  49, TERM11, AirportBlock::Nothing, 50 }, {  49, TERM12, AirportBlock::Nothing, 50 }, {  49, TERM13, AirportBlock::Nothing, 50 }, {  49, TERM14, AirportBlock::Nothing, 50 }, {  49, TERM15, AirportBlock::Nothing, 50 }, {  49, TERM16, AirportBlock::Nothing, 50 }, {  49, TO_ALL, AirportBlock::Nothing, 48 },  // Rollweg 5/7
	{  50, TERMGROUP, AirportBlock::Nothing, 50 }, {  50, HANGAR, AirportBlock::Nothing, 51 }, {  50, TERM2, AirportBlock::Nothing, 63 }, {  50, TERM3, AirportBlock::Nothing, 63 }, {  50, TERM4, AirportBlock::Nothing, 63 }, {  50, TERM7, AirportBlock::Nothing, 63 }, {  50, TERM8, AirportBlock::Nothing, 63 }, {  50, TERM9, AirportBlock::Nothing, 63 }, {  50, TERM10, AirportBlock::Nothing, 63 }, {  50, TERM11, AirportBlock::Nothing, 63 }, {  50, TERM12, AirportBlock::Nothing, 63 }, {  50, TERM13, AirportBlock::Nothing, 63 }, {  50, TERM14, AirportBlock::Nothing, 63 }, {  50, TERM15, AirportBlock::Nothing, 63 }, {  50, TERM16, AirportBlock::Nothing, 63 }, {  50, TO_ALL, AirportBlock::Nothing, 49 },  // Rollweg 6/7
	{  51, TERMGROUP, AirportBlock::Nothing, 51 }, {  51, HANGAR, AirportBlock::Nothing, 52 }, {  51, TO_ALL, AirportBlock::Nothing, 50 },  // Rollweg 7/7
	{  52, TERMGROUP, AirportBlock::Nothing, 52 }, {  52, HANGAR, AirportBlock::Nothing, 53 }, {  52, TERM12, AirportBlock::Nothing, 53 }, {  52, TERM16, AirportBlock::Nothing, 53 }, {  52, TO_ALL, AirportBlock::Nothing, 51 },  // Rollweg 8/7
	{  53, TERMGROUP, AirportBlock::Nothing, 53 }, {  53, HANGAR, AirportBlock::Nothing, 54 }, {  53, TERM11, AirportBlock::Nothing, 54 }, {  53, TERM12, AirportBlock::Nothing, 54 }, {  53, TERM14, AirportBlock::Nothing, 54 }, {  53, TERM15, AirportBlock::Nothing, 54 }, {  53, TERM16, AirportBlock::Nothing, 54 }, {  53, TO_ALL, AirportBlock::Nothing, 52 },  // Rollweg 9/7
	{  54, TERMGROUP, AirportBlock::Nothing, 54 }, {  54, HANGAR, AirportBlock::Nothing, 55 }, {  54, TERM10, AirportBlock::Nothing, 55 }, {  54, TERM11, AirportBlock::Nothing, 55 }, {  54, TERM12, AirportBlock::Nothing, 55 }, {  54, TERM13, AirportBlock::Nothing, 55 }, {  54, TERM14, AirportBlock::Nothing, 55 }, {  54, TERM15, AirportBlock::Nothing, 55 }, {  54, TERM16, AirportBlock::Nothing, 55 }, {  54, TO_ALL, AirportBlock::Nothing, 53 },  // Rollweg 10/7
	{  55, TERMGROUP, AirportBlock::Nothing, 55 }, {  55, HANGAR, AirportBlock::Nothing, 64 }, {  55, TERM9, AirportBlock::Nothing, 64 }, {  55, TERM10, AirportBlock::Nothing, 64 }, {  55, TERM11, AirportBlock::Nothing, 64 }, {  55, TERM12, AirportBlock::Nothing, 64 }, {  55, TERM13, AirportBlock::Nothing, 64 }, {  55, TERM14, AirportBlock::Nothing, 64 }, {  55, TERM15, AirportBlock::Nothing, 64 }, {  55, TERM16, AirportBlock::Nothing, 64 }, {  55, TO_ALL, AirportBlock::Nothing, 54 },  // Rollweg 11/7
	{  56, TERMGROUP, AirportBlock::Nothing, 56 }, {  56, TO_ALL, AirportBlock::Nothing, 21 },  // Rollweg 1/2
	{  57, TERMGROUP, AirportBlock::Nothing, 57 }, {  57, TO_ALL, AirportBlock::Nothing, 26 },  // Rollweg 6/2
	{  58, TERMGROUP, AirportBlock::Nothing, 58 }, {  58, HANGAR, AirportBlock::Nothing, 0 }, {  58, TO_ALL, AirportBlock::Nothing, 31 },  // Rollweg 11/2
	{  59, TERMGROUP, AirportBlock::Nothing, 59 }, {  59, HANGAR, AirportBlock::Nothing, 21 }, {  59, TERM1, AirportBlock::Nothing, 21 }, {  59, TERM2, AirportBlock::Nothing, 21 }, {  59, TERM3, AirportBlock::Nothing, 21 }, {  59, TERM4, AirportBlock::Nothing, 21 }, {  59, TERM7, AirportBlock::Nothing, 21 }, {  59, TERM8, AirportBlock::Nothing, 21 }, {  59, TERM9, AirportBlock::Nothing, 21 }, {  59, TERM10, AirportBlock::Nothing, 21 }, {  59, TERM11, AirportBlock::Nothing, 21 }, {  59, TERM12, AirportBlock::Nothing, 21 }, {  59, TERM13, AirportBlock::Nothing, 21 }, {  59, TO_ALL, AirportBlock::Nothing, 33 },  // Rollweg 1/4
	{  60, TERMGROUP, AirportBlock::Nothing, 60 }, {  60, HANGAR, AirportBlock::Nothing, 26 }, {  60, TERM1, AirportBlock::Nothing, 26 }, {  60, TERM2, AirportBlock::Nothing, 26 }, {  60, TERM3, AirportBlock::Nothing, 26 }, {  60, TERM4, AirportBlock::Nothing, 26 }, {  60, TERM7, AirportBlock::Nothing, 26 }, {  60, TERM8, AirportBlock::Nothing, 26 }, {  60, TERM9, AirportBlock::Nothing, 26 }, {  60, TERM10, AirportBlock::Nothing, 26 }, {  60, TERM11, AirportBlock::Nothing, 26 }, {  60, TERM12, AirportBlock::Nothing, 26 }, {  60, TERM13, AirportBlock::Nothing, 26 }, {  60, TO_ALL, AirportBlock::Nothing, 38 },  // Rollweg 6/4
	{  61, TERMGROUP, AirportBlock::Nothing, 61 }, {  61, HANGAR, AirportBlock::Nothing, 31 }, {  61, TERM1, AirportBlock::Nothing, 31 }, {  61, TERM2, AirportBlock::Nothing, 31 }, {  61, TERM3, AirportBlock::Nothing, 31 }, {  61, TERM4, AirportBlock::Nothing, 31 }, {  61, TERM7, AirportBlock::Nothing, 31 }, {  61, TERM8, AirportBlock::Nothing, 31 }, {  61, TERM9, AirportBlock::Nothing, 31 }, {  61, TERM10, AirportBlock::Nothing, 31 }, {  61, TERM11, AirportBlock::Nothing, 31 }, {  61, TERM12, AirportBlock::Nothing, 31 }, {  61, TERM13, AirportBlock::Nothing, 31 }, {  61, TO_ALL, AirportBlock::Nothing, 43 },  // Rollweg 11/4
	{  62, TERMGROUP, AirportBlock::Nothing, 62 }, {  62, HANGAR, AirportBlock::Nothing, 33 }, {  62, TERM1, AirportBlock::Nothing, 33 }, {  62, TERM2, AirportBlock::Nothing, 33 }, {  62, TERM3, AirportBlock::Nothing, 33 }, {  62, TERM4, AirportBlock::Nothing, 33 }, {  62, TERM5, AirportBlock::Nothing, 33 }, {  62, TERM6, AirportBlock::Nothing, 33 }, {  62, TERM7, AirportBlock::Nothing, 33 }, {  62, TERM8, AirportBlock::Nothing, 33 }, {  62, TERM9, AirportBlock::Nothing, 33 }, {  62, TERM10, AirportBlock::Nothing, 33 }, {  62, TERM11, AirportBlock::Nothing, 33 }, {  62, TERM12, AirportBlock::Nothing, 33 }, {  62, TERM13, AirportBlock::Nothing, 33 }, {  62, TERM14, AirportBlock::Nothing, 33 }, {  62, TERM15, AirportBlock::Nothing, 33 }, {  62, TERM16, AirportBlock::Nothing, 33 }, {  62, HELIPAD1, AirportBlock::Nothing, 33 }, {  62, HELIPAD2, AirportBlock::Nothing, 33 }, {  62, HELITAKEOFF, AirportBlock::Nothing, 33 }, {  62, TO_ALL, AirportBlock::Nothing, 45 },  // Rollweg 1/6
	{  63, TERMGROUP, AirportBlock::Nothing, 63 }, {  63, HANGAR, AirportBlock::Nothing, 38 }, {  63, TERM1, AirportBlock::Nothing, 38 }, {  63, TERM2, AirportBlock::Nothing, 38 }, {  63, TERM3, AirportBlock::Nothing, 38 }, {  63, TERM4, AirportBlock::Nothing, 38 }, {  63, TERM5, AirportBlock::Nothing, 38 }, {  63, TERM6, AirportBlock::Nothing, 38 }, {  63, TERM7, AirportBlock::Nothing, 38 }, {  63, TERM8, AirportBlock::Nothing, 38 }, {  63, TERM9, AirportBlock::Nothing, 38 }, {  63, TERM10, AirportBlock::Nothing, 38 }, {  63, TERM11, AirportBlock::Nothing, 38 }, {  63, TERM12, AirportBlock::Nothing, 38 }, {  63, TERM13, AirportBlock::Nothing, 38 }, {  63, TERM14, AirportBlock::Nothing, 38 }, {  63, TERM15, AirportBlock::Nothing, 38 }, {  63, TERM16, AirportBlock::Nothing, 38 }, {  63, HELIPAD1, AirportBlock::Nothing, 38 }, {  63, HELIPAD2, AirportBlock::Nothing, 38 }, {  63, HELITAKEOFF, AirportBlock::Nothing, 38 }, {  63, TO_ALL, AirportBlock::Nothing, 50 },  // Rollweg 6/6
	{  64, TERMGROUP, AirportBlock::Nothing, 64 }, {  64, HANGAR, AirportBlock::Nothing, 43 }, {  64, TERM1, AirportBlock::Nothing, 43 }, {  64, TERM2, AirportBlock::Nothing, 43 }, {  64, TERM3, AirportBlock::Nothing, 43 }, {  64, TERM4, AirportBlock::Nothing, 43 }, {  64, TERM5, AirportBlock::Nothing, 43 }, {  64, TERM6, AirportBlock::Nothing, 43 }, {  64, TERM7, AirportBlock::Nothing, 43 }, {  64, TERM8, AirportBlock::Nothing, 43 }, {  64, TERM9, AirportBlock::Nothing, 43 }, {  64, TERM10, AirportBlock::Nothing, 43 }, {  64, TERM11, AirportBlock::Nothing, 43 }, {  64, TERM12, AirportBlock::Nothing, 43 }, {  64, TERM13, AirportBlock::Nothing, 43 }, {  64, TERM14, AirportBlock::Nothing, 43 }, {  64, TERM15, AirportBlock::Nothing, 43 }, {  64, TERM16, AirportBlock::Nothing, 43 }, {  64, HELIPAD1, AirportBlock::Nothing, 43 }, {  64, HELIPAD2, AirportBlock::Nothing, 43 }, {  64, HELITAKEOFF, AirportBlock::Nothing, 43 }, {  64, TO_ALL, AirportBlock::Nothing, 55 },  // Rollweg 11/6
	{  65, TERMGROUP, AirportBlock::Nothing, 65 }, {  65, TO_ALL, AirportBlock::Nothing, 66 },  // Rollweg 0/10
	{  66, TERMGROUP, AirportBlock::Nothing, 66 }, {  66, HANGAR, AirportBlock::Nothing, 67 }, {  66, TO_ALL, AirportBlock::Nothing, 45 },  // Rollweg 1/10
	{  67, TERMGROUP, AirportBlock::Nothing, 67 }, {  67, HANGAR, AirportBlock::Nothing, 68 }, {  67, TO_ALL, AirportBlock::Nothing, 66 },  // Rollweg 2/10
	{  68, TERMGROUP, AirportBlock::Nothing, 68 }, {  68, HANGAR, AirportBlock::Nothing, 69 }, {  68, TERM12, AirportBlock::Nothing, 69 }, {  68, TERM16, AirportBlock::Nothing, 69 }, {  68, TO_ALL, AirportBlock::Nothing, 67 },  // Rollweg 3/10
	{  69, TERMGROUP, AirportBlock::Nothing, 69 }, {  69, HANGAR, AirportBlock::Nothing, 70 }, {  69, TERM11, AirportBlock::Nothing, 70 }, {  69, TERM12, AirportBlock::Nothing, 70 }, {  69, TERM14, AirportBlock::Nothing, 70 }, {  69, TERM15, AirportBlock::Nothing, 70 }, {  69, TERM16, AirportBlock::Nothing, 70 }, {  69, TO_ALL, AirportBlock::Nothing, 68 },  // Rollweg 4/10
	{  70, TERMGROUP, AirportBlock::Nothing, 70 }, {  70, HANGAR, AirportBlock::Nothing, 71 }, {  70, TERM10, AirportBlock::Nothing, 71 }, {  70, TERM11, AirportBlock::Nothing, 71 }, {  70, TERM12, AirportBlock::Nothing, 71 }, {  70, TERM13, AirportBlock::Nothing, 71 }, {  70, TERM14, AirportBlock::Nothing, 71 }, {  70, TERM15, AirportBlock::Nothing, 71 }, {  70, TERM16, AirportBlock::Nothing, 71 }, {  70, TO_ALL, AirportBlock::Nothing, 69 },  // Rollweg 5/10
	{  71, TERMGROUP, AirportBlock::Nothing, 71 }, {  71, HANGAR, AirportBlock::Nothing, 72 }, {  71, TERM9, AirportBlock::Nothing, 72 }, {  71, TERM10, AirportBlock::Nothing, 72 }, {  71, TERM11, AirportBlock::Nothing, 72 }, {  71, TERM12, AirportBlock::Nothing, 72 }, {  71, TERM13, AirportBlock::Nothing, 72 }, {  71, TERM14, AirportBlock::Nothing, 72 }, {  71, TERM15, AirportBlock::Nothing, 72 }, {  71, TERM16, AirportBlock::Nothing, 72 }, {  71, TO_ALL, AirportBlock::Nothing, 70 },  // Rollweg 6/10
	{  72, TERMGROUP, AirportBlock::Nothing, 72 }, {  72, HANGAR, AirportBlock::Nothing, 73 }, {  72, TERM9, AirportBlock::Nothing, 73 }, {  72, TERM10, AirportBlock::Nothing, 73 }, {  72, TERM11, AirportBlock::Nothing, 73 }, {  72, TERM12, AirportBlock::Nothing, 73 }, {  72, TERM13, AirportBlock::Nothing, 73 }, {  72, TERM14, AirportBlock::Nothing, 73 }, {  72, TERM15, AirportBlock::Nothing, 73 }, {  72, TERM16, AirportBlock::Nothing, 73 }, {  72, TO_ALL, AirportBlock::Nothing, 71 },  // Rollweg 7/10
	{  73, TERMGROUP, AirportBlock::Nothing, 73 }, {  73, HANGAR, AirportBlock::Nothing, 74 }, {  73, TERM4, AirportBlock::Nothing, 74 }, {  73, TERM8, AirportBlock::Nothing, 74 }, {  73, TERM9, AirportBlock::Nothing, 74 }, {  73, TERM10, AirportBlock::Nothing, 74 }, {  73, TERM11, AirportBlock::Nothing, 74 }, {  73, TERM12, AirportBlock::Nothing, 74 }, {  73, TERM13, AirportBlock::Nothing, 74 }, {  73, TERM14, AirportBlock::Nothing, 74 }, {  73, TERM15, AirportBlock::Nothing, 74 }, {  73, TERM16, AirportBlock::Nothing, 74 }, {  73, TO_ALL, AirportBlock::Nothing, 72 },  // Rollweg 8/10
	{  74, TERMGROUP, AirportBlock::Nothing, 74 }, {  74, HANGAR, AirportBlock::Nothing, 75 }, {  74, TERM2, AirportBlock::Nothing, 75 }, {  74, TERM3, AirportBlock::Nothing, 75 }, {  74, TERM4, AirportBlock::Nothing, 75 }, {  74, TERM7, AirportBlock::Nothing, 75 }, {  74, TERM8, AirportBlock::Nothing, 75 }, {  74, TERM9, AirportBlock::Nothing, 75 }, {  74, TERM10, AirportBlock::Nothing, 75 }, {  74, TERM11, AirportBlock::Nothing, 75 }, {  74, TERM12, AirportBlock::Nothing, 75 }, {  74, TERM13, AirportBlock::Nothing, 75 }, {  74, TERM14, AirportBlock::Nothing, 75 }, {  74, TERM15, AirportBlock::Nothing, 75 }, {  74, TERM16, AirportBlock::Nothing, 75 }, {  74, HELIPAD2, AirportBlock::Nothing, 75 }, {  74, TO_ALL, AirportBlock::Nothing, 73 },  // Rollweg 9/10
	{  75, TERMGROUP, AirportBlock::Nothing, 75 }, {  75, HANGAR, AirportBlock::Nothing, 76 }, {  75, TERM2, AirportBlock::Nothing, 76 }, {  75, TERM3, AirportBlock::Nothing, 76 }, {  75, TERM4, AirportBlock::Nothing, 76 }, {  75, TERM6, AirportBlock::Nothing, 76 }, {  75, TERM7, AirportBlock::Nothing, 76 }, {  75, TERM8, AirportBlock::Nothing, 76 }, {  75, TERM9, AirportBlock::Nothing, 76 }, {  75, TERM10, AirportBlock::Nothing, 76 }, {  75, TERM11, AirportBlock::Nothing, 76 }, {  75, TERM12, AirportBlock::Nothing, 76 }, {  75, TERM13, AirportBlock::Nothing, 76 }, {  75, TERM14, AirportBlock::Nothing, 76 }, {  75, TERM15, AirportBlock::Nothing, 76 }, {  75, TERM16, AirportBlock::Nothing, 76 }, {  75, HELIPAD2, AirportBlock::Nothing, 76 }, {  75, TO_ALL, AirportBlock::Nothing, 74 },  // Rollweg 10/10
	{  76, TERMGROUP, AirportBlock::Nothing, 76 }, {  76, HANGAR, AirportBlock::Nothing, 55 }, {  76, TERM1, AirportBlock::Nothing, 55 }, {  76, TERM2, AirportBlock::Nothing, 55 }, {  76, TERM3, AirportBlock::Nothing, 55 }, {  76, TERM4, AirportBlock::Nothing, 55 }, {  76, TERM5, AirportBlock::Nothing, 55 }, {  76, TERM6, AirportBlock::Nothing, 55 }, {  76, TERM7, AirportBlock::Nothing, 55 }, {  76, TERM8, AirportBlock::Nothing, 55 }, {  76, TERM9, AirportBlock::Nothing, 55 }, {  76, TERM10, AirportBlock::Nothing, 55 }, {  76, TERM11, AirportBlock::Nothing, 55 }, {  76, TERM12, AirportBlock::Nothing, 55 }, {  76, TERM13, AirportBlock::Nothing, 55 }, {  76, TERM14, AirportBlock::Nothing, 55 }, {  76, TERM15, AirportBlock::Nothing, 55 }, {  76, TERM16, AirportBlock::Nothing, 55 }, {  76, HELIPAD1, AirportBlock::Nothing, 55 }, {  76, HELIPAD2, AirportBlock::Nothing, 55 }, {  76, HELITAKEOFF, AirportBlock::Nothing, 55 }, {  76, TO_ALL, AirportBlock::Nothing, 75 },  // Rollweg 11/10
	{  77, TERMGROUP, AirportBlock::AirportEntrance, 77 }, {  77, TO_ALL, AirportBlock::Nothing, 56 },  // Flughafeneinfahrt
	{  78, TERMGROUP, AirportBlock::OutWay, 78 }, {  78, HANGAR, AirportBlock::Nothing, 45 }, {  78, TERM1, AirportBlock::Nothing, 45 }, {  78, TERM2, AirportBlock::Nothing, 45 }, {  78, TERM3, AirportBlock::Nothing, 45 }, {  78, TERM4, AirportBlock::Nothing, 45 }, {  78, TERM5, AirportBlock::Nothing, 45 }, {  78, TERM6, AirportBlock::Nothing, 45 }, {  78, TERM7, AirportBlock::Nothing, 45 }, {  78, TERM8, AirportBlock::Nothing, 45 }, {  78, TERM9, AirportBlock::Nothing, 45 }, {  78, TERM10, AirportBlock::Nothing, 45 }, {  78, TERM11, AirportBlock::Nothing, 45 }, {  78, TERM12, AirportBlock::Nothing, 45 }, {  78, TERM13, AirportBlock::Nothing, 45 }, {  78, TERM14, AirportBlock::Nothing, 45 }, {  78, TERM15, AirportBlock::Nothing, 45 }, {  78, TERM16, AirportBlock::Nothing, 45 }, {  78, HELIPAD1, AirportBlock::Nothing, 45 }, {  78, HELIPAD2, AirportBlock::Nothing, 45 }, {  78, TAKEOFF, AirportBlock::RunwayOut, 79 }, {  78, HELITAKEOFF, AirportBlock::Nothing, 45 }, {  78, TO_ALL, AirportBlock::Nothing, 78 },  // Weg zur Startbahn
	{  79, TAKEOFF, AirportBlock::RunwayOut, 80 }, {  79, TO_ALL, AirportBlock::RunwayOut, 80 },  // Beschleunigen auf der Startbahn
	{  80, TO_ALL, AirportBlock::RunwayOut, 81 },  // Startbahn freigeben
	{  81, STARTTAKEOFF, AirportBlock::RunwayOut, 82 },  // Ende der Startbahn
	{  82, ENDTAKEOFF, AirportBlock::Nothing, 0 },  // Abheben
	{  83, FLYING, AirportBlock::Nothing, 88 }, {  83, LANDING, AirportBlock::RunwayIn, 84 }, {  83, HELILANDING, AirportBlock::PreHelipad, 92 },  // Anflugpunkt
	{  84, LANDING, AirportBlock::RunwayIn, 85 },  // Landeanflug
	{  85, TO_ALL, AirportBlock::RunwayIn, 86 },  // Ausrollen
	{  86, TO_ALL, AirportBlock::RunwayIn, 87 },  // Wenden nach der Landung
	{  87, ENDLANDING, AirportBlock::AirportEntrance, 77 }, {  87, TO_ALL, AirportBlock::AirportEntrance, 77 },  // Rollen von der Bahn
	{  88, TO_ALL, AirportBlock::Nothing, 89 },  // Warteschleife Sued
	{  89, TO_ALL, AirportBlock::Nothing, 90 },  // Warteschleife Ost
	{  90, TO_ALL, AirportBlock::Nothing, 91 },  // Warteschleife Nord
	{  91, TO_ALL, AirportBlock::Nothing, 83 },  // Warteschleife West
	{  92, HELILANDING, AirportBlock::PreHelipad, 92 }, {  92, HELIENDLANDING, AirportBlock::PreHelipad, 92 }, {  92, HELIPAD1, AirportBlock::Nothing, 93 }, {  92, HELIPAD2, AirportBlock::Nothing, 94 }, {  92, HANGAR, AirportBlock::Nothing, 45 }, {  92, FLYING, AirportBlock::Nothing, 83 },  // Warteraum vor den Helipads
	{  93, TO_ALL, AirportBlock::Nothing, 95 }, {  93, FLYING, AirportBlock::Nothing, 83 },  // Anflug Helipad 1
	{  94, TO_ALL, AirportBlock::Nothing, 96 }, {  94, FLYING, AirportBlock::Nothing, 83 },  // Anflug Helipad 2
	{  95, TERMGROUP, AirportBlock::Nothing, 0 }, {  95, HELIPAD1, AirportBlock::Helipad1, 18 },  // Landen Helipad 1
	{  96, TERMGROUP, AirportBlock::Nothing, 0 }, {  96, HELIPAD2, AirportBlock::Helipad2, 19 },  // Landen Helipad 2
	{  97, HELITAKEOFF, AirportBlock::Helipad1, 0 },  // Abheben Helipad 1
	{  98, HELITAKEOFF, AirportBlock::Helipad2, 0 },  // Abheben Helipad 2
	{ MAX_ELEMENTS, 0, {}, 0 } // Endmarke - nicht entfernen
};

/** Fork: eine einzige Terminalgruppe mit allen 16 Plaetzen. */
static const uint8_t _airport_terminal_mega[] = {1, 16};

/** Fork: Anflugpunkte je Himmelsrichtung. */
static const uint8_t _airport_entries_mega[] = {89, 90, 91, 92};

#endif /* AIRPORT_MOVEMENT_H */
