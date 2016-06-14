//
//  6560.cpp
//  Clock Signal
//
//  Created by Thomas Harte on 05/06/2016.
//  Copyright © 2016 Thomas Harte. All rights reserved.
//

#include "6560.hpp"

using namespace MOS;

MOS6560::MOS6560() :
	_crt(new Outputs::CRT::CRT(65*4, 4, Outputs::CRT::NTSC60, 1)),
	_horizontal_counter(0),
	_vertical_counter(0),
	_cycles_since_speaker_update(0),
	_is_odd_frame(false)
{
	_crt->set_composite_sampling_function(
		"float composite_sample(usampler2D texID, vec2 coordinate, vec2 iCoordinate, float phase, float amplitude)"
		"{"
			"uint c = texture(texID, coordinate).r;"
			"float y = float(c >> 4) / 4.0;"
			"uint yC = c & 15u;"
			"float phaseOffset = 6.283185308 * float(yC) / 16.0;"

//			"float chroma = step(mod(phase + phaseOffset + 0.785398163397448, 6.283185308), 3.141592654);"
			"float chroma = cos(phase + phaseOffset);"
			"return mix(y, step(yC, 14) * chroma, amplitude);"
		"}");

	// set up colours table
	// 0
	// 2, 6, 9, B,
	// 4, 5, 8, A, C, E
	// 3, 7, D, F
	// 1
	uint8_t luminances[16] = {		// range is 0–4
		0, 4, 1, 3, 2, 2, 1, 3,
		2, 1, 2, 1, 2, 3, 2, 3
	};
//	uint8_t pal_chrominances[16] = {	// range is 0–15; 15 is a special case meaning "no chrominance"
//		15, 15, 5, 13, 2, 10, 0, 8,
//		6, 7, 5, 13, 2, 10, 0, 8,
//	};
	uint8_t ntsc_chrominances[16] = {
		15, 15, 2, 10, 4, 12, 6, 14,
		0, 8, 2, 10, 4, 12, 6, 14,
	};
	for(int c = 0; c < 16; c++)
	{
		_colours[c] = (uint8_t)((luminances[c] << 4) | ntsc_chrominances[c]);
	}

	// show only the centre
	_crt->set_visible_area(_crt->get_rect_for_area(16, 237, 11*4, 55*4, 4.0f / 3.0f));
	_speaker.set_input_rate(255681.75);	// assuming NTSC; clock rate / 4
}

void MOS6560::set_register(int address, uint8_t value)
{
	address &= 0xf;
	_registers[address] = value;
	switch(address)
	{
		case 0x0:
			_interlaced = !!(value&0x80);
			_first_column_location = value & 0x7f;
		break;

		case 0x1:
			_first_row_location = value;
		break;

		case 0x2:
			_number_of_columns = value & 0x7f;
			_video_matrix_start_address = (uint16_t)((_video_matrix_start_address & 0x3c00) | ((value & 0x80) << 2));
		break;

		case 0x3:
			_number_of_rows = (value >> 1)&0x3f;
			_tall_characters = !!(value&0x01);
		break;

		case 0x5:
			_character_cell_start_address = (uint16_t)((value & 0x0f) << 10);
			_video_matrix_start_address = (uint16_t)((_video_matrix_start_address & 0x0200) | ((value & 0xf0) << 6));
		break;

		case 0xa:
		case 0xb:
		case 0xc:
		case 0xd:
			update_audio();
			_speaker.set_control(address - 0xa, value);
		break;

		case 0xe:
			update_audio();
			_auxiliary_colour = _colours[value >> 4];
			_speaker.set_volume(value & 0xf);
		break;

		case 0xf:
			if(_this_state == State::Border)
			{
				output_border(_cycles_in_state * 4);
				_cycles_in_state = 0;
			}
			_invertedCells = !!((value >> 3)&1);
			_borderColour = _colours[value & 0x07];
			_backgroundColour = _colours[value >> 4];
		break;

		// TODO: audio, primarily

		default:
		break;
	}
}

uint8_t MOS6560::get_register(int address)
{
	address &= 0xf;
	switch(address)
	{
		default: return _registers[address];
		case 0x03: return (uint8_t)(_vertical_counter << 7) | (_registers[3] & 0x7f);
		case 0x04: return (_vertical_counter >> 1) & 0xff;
	}
}

void MOS6560::output_border(unsigned int number_of_cycles)
{
	uint8_t *colour_pointer = _crt->allocate_write_area(1);
	if(colour_pointer) *colour_pointer = _borderColour;
	_crt->output_level(number_of_cycles);
}

uint16_t MOS6560::get_address()
{
	_cycles_since_speaker_update++;

	_horizontal_counter++;
	if(_horizontal_counter == 65)
	{
		_horizontal_counter = 0;
		_vertical_counter++;
		_column_counter = -1;

		if(_vertical_counter == (_interlaced ? (_is_odd_frame ? 262 : 263) : 261))
		{
			_is_odd_frame ^= true;
			_vertical_counter = 0;
			_row_counter = -1;
		}

		if(_row_counter >= 0)
		{
			_row_counter++;
			if(_row_counter == _number_of_rows*(_tall_characters ? 16 : 8)) _row_counter = -1;
		}
		else if(_vertical_counter == _first_row_location * 2)
		{
			_video_matrix_line_address_counter = _video_matrix_start_address;
			_row_counter = 0;
		}
	}

	if(_column_counter >= 0)
	{
		_column_counter++;
		if(_column_counter == _number_of_columns*2)
		{
			_column_counter = -1;
			if((_row_counter&(_tall_characters ? 15 : 7)) == (_tall_characters ? 15 : 7))
			{
				_video_matrix_line_address_counter = _video_matrix_address_counter;
			}
		}
	}
	else if(_horizontal_counter == _first_column_location)
	{
		_column_counter = 0;
		_video_matrix_address_counter = _video_matrix_line_address_counter;
	}

	// determine output state; colour burst and sync timing are currently a guess
	if(_horizontal_counter > 61) _this_state = State::ColourBurst;
	else if(_horizontal_counter > 57) _this_state = State::Sync;
	else
	{
		_this_state = (_column_counter >= 0 && _row_counter >= 0) ? State::Pixels : State::Border;
	}

	// apply vertical sync
	if(
		(_vertical_counter < 3 && (_is_odd_frame || !_interlaced)) ||
		(_interlaced &&
			(
				(_vertical_counter == 0 && _horizontal_counter > 32) ||
				(_vertical_counter == 1) || (_vertical_counter == 2) ||
				(_vertical_counter == 3 && _horizontal_counter <= 32)
			)
		))
		_this_state = State::Sync;

	// update the CRT
	if(_this_state != _output_state)
	{
		switch(_output_state)
		{
			case State::Sync:			_crt->output_sync(_cycles_in_state * 4);										break;
			case State::ColourBurst:	_crt->output_colour_burst(_cycles_in_state * 4, _is_odd_frame ? 128 : 0, 0);	break;
			case State::Border:			output_border(_cycles_in_state * 4);											break;
			case State::Pixels:			_crt->output_data(_cycles_in_state * 4, 1);										break;
		}
		_output_state = _this_state;
		_cycles_in_state = 0;

		pixel_pointer = nullptr;
		if(_output_state == State::Pixels)
		{
			pixel_pointer = _crt->allocate_write_area(260);
		}
	}
	_cycles_in_state++;

	// compute the address
	if(_this_state == State::Pixels)
	{
		/*
			Per http://tinyvga.com/6561 :

			The basic video timing is very simple.  For
			every character the VIC-I is about to display, it first fetches the
			character code and colour, then the character appearance (from the
			character generator memory).  The character codes are read on every
			raster line, thus making every line a "bad line".  When the raster
			beam is outside of the text window, the videochip reads from $001c for
			most time.  (Some videochips read from $181c instead.)  The address
			occasionally varies, but it might also be due to a flaky bus.  (By
			reading from unconnected address space, such as $9100-$910f, you can
			read the data fetched by the videochip on the previous clock cycle.)
		*/
		if(_column_counter&1)
		{
			return _character_cell_start_address + (_character_code*(_tall_characters ? 16 : 8)) + (_row_counter&(_tall_characters ? 15 : 7));
		}
		else
		{
			uint16_t result = _video_matrix_address_counter;
			_video_matrix_address_counter++;
			return result;
		}
	}

	return 0x1c;
}

void MOS6560::set_graphics_value(uint8_t value, uint8_t colour_value)
{
	// TODO: this isn't correct, as _character_value will be
	// accessed second, then output will roll over. Probably it's
	// correct (given the delays upstream) to output all 8 on an &1
	// and to adjust the signalling to the CRT above?
	if(_this_state == State::Pixels)
	{
		if(_column_counter&1)
		{
			_character_value = value;

			if(pixel_pointer)
			{
				uint8_t cell_colour = _colours[_character_colour & 0x7];
				if(!(_character_colour&0x8))
				{
					pixel_pointer[0] = ((_character_value >> 7)&1) ? cell_colour : _backgroundColour;
					pixel_pointer[1] = ((_character_value >> 6)&1) ? cell_colour : _backgroundColour;
					pixel_pointer[2] = ((_character_value >> 5)&1) ? cell_colour : _backgroundColour;
					pixel_pointer[3] = ((_character_value >> 4)&1) ? cell_colour : _backgroundColour;
					pixel_pointer[4] = ((_character_value >> 3)&1) ? cell_colour : _backgroundColour;
					pixel_pointer[5] = ((_character_value >> 2)&1) ? cell_colour : _backgroundColour;
					pixel_pointer[6] = ((_character_value >> 1)&1) ? cell_colour : _backgroundColour;
					pixel_pointer[7] = ((_character_value >> 0)&1) ? cell_colour : _backgroundColour;
				}
				else
				{
					uint8_t colours[4] = {_backgroundColour, _borderColour, cell_colour, _auxiliary_colour};
					pixel_pointer[0] =
					pixel_pointer[1] = colours[(_character_value >> 6)&3];
					pixel_pointer[2] =
					pixel_pointer[3] = colours[(_character_value >> 4)&3];
					pixel_pointer[4] =
					pixel_pointer[5] = colours[(_character_value >> 2)&3];
					pixel_pointer[6] =
					pixel_pointer[7] = colours[(_character_value >> 0)&3];
				}
				pixel_pointer += 8;
			}
		}
		else
		{
			_character_code = value;
			_character_colour = colour_value;
		}
	}
}

void MOS6560::update_audio()
{
	_speaker.run_for_cycles(_cycles_since_speaker_update >> 2);
	_cycles_since_speaker_update &= 3;
}

#pragma mark - Audio

MOS6560Speaker::MOS6560Speaker() :
	_volume(0),
	_control_registers{0, 0, 0, 0},
	_shift_registers{0, 0, 0, 0},
	_counters{2, 1, 0, 0}	// create a slight phase offset for the three channels
{
}

void MOS6560Speaker::set_volume(uint8_t volume)
{
	_volume = volume;
}

void MOS6560Speaker::set_control(int channel, uint8_t value)
{
	_control_registers[channel] = value;
}

// Source: VICE. Not original.
static uint8_t noise_pattern[] = {
	0x07, 0x1e, 0x1e, 0x1c, 0x1c, 0x3e, 0x3c, 0x38, 0x78, 0xf8, 0x7c, 0x1e, 0x1f, 0x8f, 0x07, 0x07,
	0xc1, 0xc0, 0xe0, 0xf1, 0xe0, 0xf0, 0xe3, 0xe1, 0xc0, 0xe0, 0x78, 0x7e, 0x3c, 0x38, 0xe0, 0xe1,
	0xc3, 0xc3, 0x87, 0xc7, 0x07, 0x1e, 0x1c, 0x1f, 0x0e, 0x0e, 0x1e, 0x0e, 0x0f, 0x0f, 0xc3, 0xc3,
	0xf1, 0xe1, 0xe3, 0xc1, 0xe3, 0xc3, 0xc3, 0xfc, 0x3c, 0x1e, 0x0f, 0x83, 0xc3, 0xc1, 0xc1, 0xc3,
	0xc3, 0xc7, 0x87, 0x87, 0xc7, 0x0f, 0x0e, 0x3c, 0x7c, 0x78, 0x3c, 0x3c, 0x3c, 0x38, 0x3e, 0x1c,
	0x7c, 0x1e, 0x3c, 0x0f, 0x0e, 0x3e, 0x78, 0xf0, 0xf0, 0xe0, 0xe1, 0xf1, 0xc1, 0xc3, 0xc7, 0xc3,
	0xe1, 0xf1, 0xe0, 0xe1, 0xf0, 0xf1, 0xe3, 0xc0, 0xf0, 0xe0, 0xf8, 0x70, 0xe3, 0x87, 0x87, 0xc0,
	0xf0, 0xe0, 0xf1, 0xe1, 0xe1, 0xc7, 0x83, 0x87, 0x83, 0x8f, 0x87, 0x87, 0xc7, 0x83, 0xc3, 0x83,
	0xc3, 0xf1, 0xe1, 0xc3, 0xc7, 0x81, 0xcf, 0x87, 0x03, 0x87, 0xc7, 0xc7, 0x87, 0x83, 0xe1, 0xc3,
	0x07, 0xc3, 0x87, 0x87, 0x07, 0x87, 0xc3, 0x87, 0x83, 0xe1, 0xc3, 0xc7, 0xc3, 0x87, 0x87, 0x8f,
	0x0f, 0x87, 0x87, 0x0f, 0xcf, 0x1f, 0x87, 0x8e, 0x0e, 0x07, 0x81, 0xc3, 0xe3, 0xc1, 0xe0, 0xf0,
	0xe0, 0xe3, 0x83, 0x87, 0x07, 0x87, 0x8e, 0x1e, 0x0f, 0x07, 0x87, 0x8f, 0x1f, 0x07, 0x87, 0xc1,
	0xf0, 0xe1, 0xe1, 0xe3, 0xc7, 0x0f, 0x03, 0x8f, 0x87, 0x0e, 0x1e, 0x1e, 0x0f, 0x87, 0x87, 0x0f,
	0x87, 0x1f, 0x0f, 0xc3, 0xc3, 0xf0, 0xf8, 0xf0, 0x70, 0xf1, 0xf0, 0xf0, 0xe1, 0xf0, 0xe0, 0x78,
	0x7c, 0x78, 0x7c, 0x70, 0x71, 0xe1, 0xe1, 0xc3, 0xc3, 0xc7, 0x87, 0x1c, 0x3c, 0x3c, 0x1c, 0x3c,
	0x7c, 0x1e, 0x1e, 0x1e, 0x1c, 0x3c, 0x78, 0xf8, 0xf8, 0xe1, 0xc3, 0x87, 0x1e, 0x1e, 0x3c, 0x3e,
	0x0f, 0x0f, 0x87, 0x1f, 0x8e, 0x0f, 0x0f, 0x8e, 0x1e, 0x1e, 0x1e, 0x1e, 0x0f, 0x0f, 0x8f, 0x87,
	0x87, 0xc3, 0x83, 0xc1, 0xe1, 0xc3, 0xc1, 0xc3, 0xc7, 0x8f, 0x0f, 0x0f, 0x0f, 0x0f, 0x83, 0xc7,
	0xc3, 0xc1, 0xe1, 0xe0, 0xf8, 0x3e, 0x3c, 0x3c, 0x3c, 0x3c, 0x3c, 0x78, 0x3e, 0x1e, 0x1e, 0x1e,
	0x0f, 0x0f, 0x0f, 0x1e, 0x0e, 0x1e, 0x1e, 0x0f, 0x0f, 0x87, 0x1f, 0x87, 0x87, 0x1c, 0x3e, 0x1f,
	0x0f, 0x0f, 0x8e, 0x3e, 0x0e, 0x3e, 0x1e, 0x1c, 0x3c, 0x7c, 0xfc, 0x38, 0x78, 0x78, 0x38, 0x78,
	0x70, 0xf8, 0x7c, 0x1e, 0x3c, 0x3c, 0x30, 0xf1, 0xf0, 0x70, 0x70, 0xe0, 0xf8, 0xf0, 0xf8, 0x78,
	0x78, 0x71, 0xe1, 0xf0, 0xe3, 0xc1, 0xf0, 0x71, 0xe3, 0xc7, 0x87, 0x8e, 0x3e, 0x0e, 0x1e, 0x3e,
	0x0f, 0x07, 0x87, 0x0c, 0x3e, 0x0f, 0x87, 0x0f, 0x1e, 0x3c, 0x3c, 0x38, 0x78, 0xf1, 0xe7, 0xc3,
	0xc3, 0xc7, 0x8e, 0x3c, 0x38, 0xf0, 0xe0, 0x7e, 0x1e, 0x3e, 0x0e, 0x0f, 0x0f, 0x0f, 0x03, 0xc3,
	0xc3, 0xc7, 0x87, 0x1f, 0x0e, 0x1e, 0x1c, 0x3c, 0x3c, 0x0f, 0x07, 0x07, 0xc7, 0xc7, 0x87, 0x87,
	0x8f, 0x0f, 0xc0, 0xf0, 0xf8, 0x60, 0xf0, 0xf0, 0xe1, 0xe3, 0xe3, 0xc3, 0xc3, 0xc3, 0x87, 0x0f,
	0x87, 0x8e, 0x1e, 0x1e, 0x3f, 0x1e, 0x0e, 0x1c, 0x3c, 0x7e, 0x1e, 0x3c, 0x38, 0x78, 0x78, 0x78,
	0x38, 0x78, 0x3c, 0xe1, 0xe3, 0x8f, 0x1f, 0x1c, 0x78, 0x70, 0x7e, 0x0f, 0x87, 0x07, 0xc3, 0xc7,
	0x0f, 0x1e, 0x3c, 0x0e, 0x0f, 0x0e, 0x1e, 0x03, 0xf0, 0xf0, 0xf1, 0xe3, 0xc1, 0xc7, 0xc0, 0xe1,
	0xe1, 0xe1, 0xe1, 0xe0, 0x70, 0xe1, 0xf0, 0x78, 0x70, 0xe3, 0xc7, 0x0f, 0xc1, 0xe1, 0xe3, 0xc3,
	0xc0, 0xf0, 0xfc, 0x1c, 0x3c, 0x70, 0xf8, 0x70, 0xf8, 0x78, 0x3c, 0x70, 0xf0, 0x78, 0x70, 0x7c,
	0x7c, 0x3c, 0x38, 0x1e, 0x3e, 0x3c, 0x7e, 0x07, 0x83, 0xc7, 0xc1, 0xc1, 0xe1, 0xc3, 0xc3, 0xc3,
	0xe1, 0xe1, 0xf0, 0x78, 0x7c, 0x3e, 0x0f, 0x1f, 0x07, 0x8f, 0x0f, 0x83, 0x87, 0xc1, 0xe3, 0xe3,
	0xc3, 0xc3, 0xe1, 0xf0, 0xf8, 0xf0, 0x3c, 0x7c, 0x3c, 0x0f, 0x8e, 0x0e, 0x1f, 0x1f, 0x0e, 0x3c,
	0x38, 0x78, 0x70, 0x70, 0xf0, 0xf0, 0xf8, 0x70, 0x70, 0x78, 0x38, 0x3c, 0x70, 0xe0, 0xf0, 0x78,
	0xf1, 0xf0, 0x78, 0x3e, 0x3c, 0x0f, 0x07, 0x0e, 0x3e, 0x1e, 0x3f, 0x1e, 0x0e, 0x0f, 0x87, 0x87,
	0x07, 0x0f, 0x07, 0xc7, 0x8f, 0x0f, 0x87, 0x1e, 0x1e, 0x1f, 0x1e, 0x1e, 0x3c, 0x1e, 0x1c, 0x3e,
	0x0f, 0x03, 0xc3, 0x81, 0xe0, 0xf0, 0xfc, 0x38, 0x3c, 0x3e, 0x0e, 0x1e, 0x1c, 0x7c, 0x1e, 0x1f,
	0x0e, 0x3e, 0x1c, 0x78, 0x78, 0x7c, 0x1e, 0x3e, 0x1e, 0x3c, 0x1f, 0x0f, 0x1f, 0x0f, 0x0f, 0x8f,
	0x1c, 0x3c, 0x78, 0xf8, 0xf0, 0xf8, 0x70, 0xf0, 0x78, 0x78, 0x3c, 0x3c, 0x78, 0x3c, 0x1f, 0x0f,
	0x07, 0x86, 0x1c, 0x1e, 0x1c, 0x1e, 0x1e, 0x1f, 0x03, 0xc3, 0xc7, 0x8e, 0x3c, 0x3c, 0x1c, 0x18,
	0xf0, 0xe1, 0xc3, 0xe1, 0xc1, 0xe1, 0xe3, 0xc3, 0xc3, 0xe3, 0xc3, 0x83, 0x87, 0x83, 0x87, 0x0f,
	0x07, 0x07, 0xe1, 0xe1, 0xe0, 0x7c, 0x78, 0x38, 0x78, 0x78, 0x3c, 0x1f, 0x0f, 0x8f, 0x0e, 0x07,
	0x0f, 0x07, 0x83, 0xc3, 0xc3, 0x81, 0xf0, 0xf8, 0xf1, 0xe0, 0xe3, 0xc7, 0x1c, 0x3e, 0x1e, 0x0f,
	0x0f, 0xc3, 0xf0, 0xf0, 0xe3, 0x83, 0xc3, 0xc7, 0x07, 0x0f, 0x0f, 0x0f, 0x0f, 0x0f, 0x07, 0x87,
	0x0f, 0x0f, 0x0e, 0x0f, 0x0f, 0x1e, 0x0f, 0x0f, 0x87, 0x87, 0x87, 0x8f, 0xc7, 0xc7, 0x83, 0x83,
	0xc3, 0xc7, 0x8f, 0x87, 0x07, 0xc3, 0x8e, 0x1e, 0x38, 0x3e, 0x3c, 0x38, 0x7c, 0x1f, 0x1c, 0x38,
	0x3c, 0x78, 0x7c, 0x1e, 0x1c, 0x3c, 0x3f, 0x1e, 0x0e, 0x3e, 0x1c, 0x3c, 0x1f, 0x0f, 0x07, 0xc3,
	0xe3, 0x83, 0x87, 0x81, 0xc1, 0xe3, 0xcf, 0x0e, 0x0f, 0x1e, 0x3e, 0x1e, 0x1f, 0x0f, 0x8f, 0xc3,
	0x87, 0x0e, 0x03, 0xf0, 0xf0, 0x70, 0xe0, 0xe1, 0xe1, 0xc7, 0x8e, 0x0f, 0x0f, 0x1e, 0x0e, 0x1e,
	0x1f, 0x1c, 0x78, 0xf0, 0xf1, 0xf1, 0xe0, 0xf1, 0xe1, 0xe1, 0xe0, 0xe0, 0xf1, 0xc1, 0xf0, 0x71,
	0xe1, 0xc3, 0x83, 0xc7, 0x83, 0xe1, 0xe1, 0xf8, 0x70, 0xf0, 0xf0, 0xf0, 0xf0, 0xf0, 0x70, 0xf8,
	0x70, 0x70, 0x61, 0xe0, 0xf0, 0xe1, 0xe0, 0x78, 0x71, 0xe0, 0xf0, 0xf8, 0x38, 0x1e, 0x1c, 0x38,
	0x70, 0xf8, 0x60, 0x78, 0x38, 0x3c, 0x3f, 0x1f, 0x0f, 0x1f, 0x0f, 0x1f, 0x87, 0x87, 0x83, 0x87,
	0x83, 0xe1, 0xe1, 0xf0, 0x78, 0xf1, 0xf0, 0x70, 0x38, 0x38, 0x70, 0xe0, 0xe3, 0xc0, 0xe0, 0xf8,
	0x78, 0x78, 0xf8, 0x38, 0xf1, 0xe1, 0xe1, 0xc3, 0x87, 0x87, 0x0e, 0x1e, 0x1f, 0x0e, 0x0e, 0x0f,
	0x0f, 0x87, 0xc3, 0x87, 0x07, 0x83, 0xc0, 0xf0, 0x38, 0x3c, 0x3c, 0x38, 0xf0, 0xfc, 0x3e, 0x1e,
	0x1c, 0x1c, 0x38, 0x70, 0xf0, 0xf1, 0xe0, 0xf0, 0xe0, 0xe0, 0xf1, 0xe3, 0xe0, 0xe1, 0xf0, 0xf0,
	0x78, 0x7c, 0x78, 0x3c, 0x78, 0x78, 0x38, 0x78, 0x78, 0x78, 0x78, 0x70, 0xe3, 0x83, 0x83, 0xe0,
	0xc3, 0xc1, 0xe1, 0xc1, 0xc1, 0xc1, 0xe3, 0xc3, 0xc7, 0x1e, 0x0e, 0x1f, 0x1e, 0x1e, 0x0f, 0x0f,
	0x0e, 0x0e, 0x0e, 0x07, 0x83, 0x87, 0x87, 0x0e, 0x07, 0x8f, 0x0f, 0x0f, 0x0f, 0x0e, 0x1c, 0x70,
	0xe1, 0xe0, 0x71, 0xc1, 0x83, 0x83, 0x87, 0x0f, 0x1e, 0x18, 0x78, 0x78, 0x7c, 0x3e, 0x1c, 0x38,
	0xf0, 0xe1, 0xe0, 0x78, 0x70, 0x38, 0x3c, 0x3e, 0x1e, 0x3c, 0x1e, 0x1c, 0x70, 0x3c, 0x38, 0x3f,
};

#define shift(r) _shift_registers[r] = (uint8_t)((_shift_registers[r] << 1) | (((_shift_registers[r]^0x80)&_control_registers[r]) >> 7));
#define increment(r) _shift_registers[r] = (_shift_registers[r]+1)%8191;
#define update(r, m, up) _counters[r]++; if((_counters[r] >> m) == 0x7f) { up(r); _counters[r] = _control_registers[r]&0x7f; }

void MOS6560Speaker::get_samples(unsigned int number_of_samples, int16_t *target)
{
	for(unsigned int c = 0; c < number_of_samples; c++)
	{
		update(0, 2, shift);
		update(1, 1, shift);
		update(2, 0, shift);
		update(3, 1, increment);

		target[c] = (
			(_shift_registers[0]&1) +
			(_shift_registers[1]&1) +
			(_shift_registers[2]&1) +
			((noise_pattern[_shift_registers[3] >> 3] >> (_shift_registers[3]&7))&(_control_registers[3] >> 7)&1)
		) * _volume * 700;
	}
}

void MOS6560Speaker::skip_samples(unsigned int number_of_samples)
{
	for(unsigned int c = 0; c < number_of_samples; c++)
	{
		update(0, 2, shift);
		update(1, 1, shift);
		update(2, 0, shift);
		update(3, 1, increment);
	}
}

#undef shift
#undef increment
#undef update
