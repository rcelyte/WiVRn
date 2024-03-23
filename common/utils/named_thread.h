/*
 * WiVRn VR streaming
 * Copyright (C) 2024  Guillaume Meunier <guillaume.meunier@centraliens.net>
 * Copyright (C) 2024  Patrick Nicolas <patricknicolas@laposte.net>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#pragma once

#include <pthread.h>
#include <thread>
#include <utility>

namespace utils
{
template <typename... Args>
std::thread named_thread(const std::string & name, Args &&... args)
{
	#ifdef __APPLE__
	std::array<char, 16> shortName = {};
	strcpy(&shortName[0], name.substr(0, 15).c_str());
	std::thread t{[](std::array<char, 16> name, auto... args) {
		pthread_setname_np(&name[0]);
		std::invoke(std::forward<Args>(args)...);
	}, shortName, std::forward<Args>(args)...};
	#else
	std::thread t{std::forward<Args>(args)...};
	pthread_setname_np(t.native_handle(), name.substr(0, 15).c_str());
	#endif
	return t;
}
} // namespace utils
