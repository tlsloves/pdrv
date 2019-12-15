/*
	(c) 2019 Samuel Tulach - All rights reserved
	Based on https://github.com/stellacaller/rust_internal_sdk

	I am using ReadProcessMemory and WriteProcessMemory because
	managing all unity object pointers that are getting destroyed
	every fking 5 seconds is not possible. Sorry.

	TODO:
	- World to screen and overlay check resolution
	- Hijack threads instead of creating them (check 
	if not in the same thread!) - done
	- Change font - done
*/

#define NULL_CHECK_RET(x) if ((uint64_t)x < 0x1000) return
#define NULL_CHECK(x) if ((uint64_t)x < 0x1000) continue

#define TEST_BUILD true
#define TEST_LOGS false
#define TARGET_THREAD 6

#include <Windows.h>
#include <d3d9.h>
#include <iostream>
#include <d3dx9.h>
#include <Dwmapi.h> 
#include <TlHelp32.h>
#include <cstdint>
#include <thread>
#include <chrono>
#include <mutex>
#include <vector>
#include <map>
#include "xor.h"
#include "memory.h"
#include "utils.hpp"
#include "globals.h"
#include "module.h"
#include "settings.h"

int width = 1920;
int height = 1080;

std::vector<base_player*> entities;
std::atomic<base_camera*> camera( nullptr );
std::atomic_bool should_exit( false );

std::mutex entity_mutex;
static base_player* local_player = nullptr;

#define GAME_WINDOW "Rust"
#define GAME_WINDOW_CLASS "UnityWndClass"

#define CENTERX (GetSystemMetrics(SM_CXSCREEN)/2)-(width/2)
#define CENTERY (GetSystemMetrics(SM_CYSCREEN)/2)-(height/2)

#define RED		D3DCOLOR_ARGB(255, 255, 0, 0)
#define GREEN	D3DCOLOR_ARGB(255, 0, 255, 0)
#define BLUE	D3DCOLOR_ARGB(255, 0, 0, 255)

LPDIRECT3D9 d3d;
LPDIRECT3DDEVICE9 d3ddev;

HWND hWnd;
const MARGINS margin = { 0,0,width,height };

LPD3DXFONT pFont;
ID3DXLine* pLine;

#include "draw.h"
#include "menu.h"
#include <excpt.h>

int filterException(int code, PEXCEPTION_POINTERS ex) {
	//std::cout << "Filtering " << std::hex << code << std::endl;
	printf("[!] Error with code %x occured\n", code);
	return EXCEPTION_EXECUTE_HANDLER;
}

LRESULT CALLBACK WindowProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam);
void initD3D(HWND hWnd)
{
	d3d = Direct3DCreate9(D3D_SDK_VERSION);

	D3DPRESENT_PARAMETERS d3dpp;

	ZeroMemory(&d3dpp, sizeof(d3dpp));
	d3dpp.Windowed = TRUE;
	d3dpp.SwapEffect = D3DSWAPEFFECT_DISCARD;
	d3dpp.hDeviceWindow = hWnd;
	d3dpp.BackBufferFormat = D3DFMT_X8R8G8B8;
	d3dpp.BackBufferWidth = width;
	d3dpp.BackBufferHeight = height;

	d3dpp.EnableAutoDepthStencil = TRUE;
	d3dpp.AutoDepthStencilFormat = D3DFMT_D16;

	d3d->CreateDevice(D3DADAPTER_DEFAULT,
		D3DDEVTYPE_HAL,
		hWnd,
		D3DCREATE_SOFTWARE_VERTEXPROCESSING,
		&d3dpp,
		&d3ddev);

	if (!pLine)
		D3DXCreateLine(d3ddev, &pLine);

	D3DXCreateFont(d3ddev, 15, 0, FW_NORMAL, 0, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, TEXT("Tahoma"), &pFont);
}

char* ToChar(int num)
{
	char* ibuff = (char*)malloc(50);
	sprintf(ibuff, "%i", num);
	return ibuff;
}

char* ToCharL(int num)
{
	char* ibuff = (char*)malloc(50);
	sprintf(ibuff, "%llx", num);
	return ibuff;
}


const std::string GetTime() 
{
	time_t     now = time(0);
	struct tm  tstruct;
	char       buf[80];
	tstruct = *localtime(&now);
	strftime(buf, sizeof(buf), "%X", &tstruct);

	return buf;
}

DWORD lasttime = 0;
static DWORD LastFrameTime = 0;
DWORD FPSLimit = 60;

void render_static() 
{
	if (!s_showmenu) 
	{
		DrawString(xorstr_("xcheats.cc"), 10, 10, 240, 0, 0, pFont);
		DrawString(GetTime().c_str(), 10, 10 + (17 * 1), 240, 0, 0, pFont);

		DWORD currenttime = timeGetTime();

		char* milliseconds = ToChar((int)(currenttime - lasttime));
		std::string mstext = std::string(milliseconds) + xorstr_(" ms");
		DrawString(mstext.c_str(), 10, 10 + (17 * 2), 240, 0, 0, pFont);
		
		free(milliseconds); // no memory leaks lol
		lasttime = timeGetTime();
	}
	if (s_crosshair) 
	{
		int size = 10;
		int cetnerx = width / 2;
		int cetnery = height / 2;
		DrawLine(cetnerx - size, cetnery, cetnerx + size, cetnery, 240, 0, 0, 255);
		DrawLine(cetnerx, cetnery - size, cetnerx, cetnery + size, 240, 0, 0, 255);
	}
	if (s_debuginfo) 
	{
		char* numbere = ToChar(entities.size());
		std::string entityt = "Ent: " + std::string(numbere);
		DrawString(entityt.c_str(), 10, 10 + (17 * 5), 240, 0, 0, pFont);
		free(numbere);

		char* numberl = ToCharL((uint64_t)local_player);
		std::string entityl = "LocalPlayer: " + std::string(numberl);
		DrawString(entityl.c_str(), 10, 10 + (17 * 6), 240, 0, 0, pFont);
		free(numberl);

		char* numberv = ToChar(VERSION);
		std::string entityv = "Version: " + std::string(numberv);
		DrawString(entityv.c_str(), 10, 10 + (17 * 7), 240, 0, 0, pFont);
		free(numberv);
	}
}

geo::vec3_t get_position(void* transforms)
{
	uint64_t transform = (uint64_t)transforms;

	auto transform_internal = read<uint64_t>(transform + 0x10);
	if (!transform_internal)
	{
		if (TEST_LOGS) printf(xorstr_("[-] Failed to get internal transform\n"));
		return {};
	}

	auto some_ptr = read<uint64_t>(transform_internal + 0x38);
	auto index = read<uint32_t>(transform_internal + 0x38 + sizeof(uint64_t));
	if (!some_ptr)
	{
		if (TEST_LOGS) printf(xorstr_("[-] Failed to get some ptr\n"));
		return {};
	}

	auto relation_array = read<uint64_t>(some_ptr + 0x18);
	if (!relation_array)
	{
		if (TEST_LOGS) printf(xorstr_("[-] Failed to read relation array\n"));
		return {};
	}

	auto dependency_index_array = read<uint64_t>(some_ptr + 0x20);
	if (!dependency_index_array)
	{
		if (TEST_LOGS) printf(xorstr_("[-] Failed to read depency index arr\n"));
		return {};
	}

	__m128i temp_0;
	__m128 xmmword_1410D1340 = { -2.f, 2.f, -2.f, 0.f };
	__m128 xmmword_1410D1350 = { 2.f, -2.f, -2.f, 0.f };
	__m128 xmmword_1410D1360 = { -2.f, -2.f, 2.f, 0.f };
	__m128 temp_1;
	__m128 temp_2;
	auto temp_main = read<__m128>(relation_array + index * 48);
	auto dependency_index = read<int32_t>(dependency_index_array + index * 4);

	while (dependency_index >= 0)
	{
		auto relation_index = 6 * dependency_index;

		temp_0 = read<__m128i>(relation_array + 8 * relation_index + 16);
		temp_1 = read<__m128>(relation_array + 8 * relation_index + 32);
		temp_2 = read<__m128>(relation_array + 8 * relation_index);

		__m128 v10 = _mm_mul_ps(temp_1, temp_main);
		__m128 v11 = _mm_castsi128_ps(_mm_shuffle_epi32(temp_0, 0));
		__m128 v12 = _mm_castsi128_ps(_mm_shuffle_epi32(temp_0, 85));
		__m128 v13 = _mm_castsi128_ps(_mm_shuffle_epi32(temp_0, -114));
		__m128 v14 = _mm_castsi128_ps(_mm_shuffle_epi32(temp_0, -37));
		__m128 v15 = _mm_castsi128_ps(_mm_shuffle_epi32(temp_0, -86));
		__m128 v16 = _mm_castsi128_ps(_mm_shuffle_epi32(temp_0, 113));
		__m128 v17 = _mm_add_ps(
			_mm_add_ps(
				_mm_add_ps(
					_mm_mul_ps(
						_mm_sub_ps(
							_mm_mul_ps(_mm_mul_ps(v11, xmmword_1410D1350), v13),
							_mm_mul_ps(_mm_mul_ps(v12, xmmword_1410D1360), v14)),
						_mm_castsi128_ps(_mm_shuffle_epi32(_mm_castps_si128(v10), -86))),
					_mm_mul_ps(
						_mm_sub_ps(
							_mm_mul_ps(_mm_mul_ps(v15, xmmword_1410D1360), v14),
							_mm_mul_ps(_mm_mul_ps(v11, xmmword_1410D1340), v16)),
						_mm_castsi128_ps(_mm_shuffle_epi32(_mm_castps_si128(v10), 85)))),
				_mm_add_ps(
					_mm_mul_ps(
						_mm_sub_ps(
							_mm_mul_ps(_mm_mul_ps(v12, xmmword_1410D1340), v16),
							_mm_mul_ps(_mm_mul_ps(v15, xmmword_1410D1350), v13)),
						_mm_castsi128_ps(_mm_shuffle_epi32(_mm_castps_si128(v10), 0))),
					v10)),
			temp_2);

		temp_main = v17;
		dependency_index = read<int32_t>(dependency_index_array + dependency_index * 4);
	}

	return read<geo::vec3_t>((uint64_t)&temp_main);
}

void render_esp() 
{	
	base_player* aiment = nullptr;
	int aimfov = 999;

	for (const auto& entityp : entities)
	{
		if (!entityp)
			continue;

		if (entityp == local_player && !TEST_BUILD)
			continue;

		base_player entity = read<base_player>((uint64_t)entityp);

		NULL_CHECK(entityp->display_name);
		NULL_CHECK(entityp->player_model);
		NULL_CHECK(entityp->model);
		NULL_CHECK(entityp->model->head_bone_transform);
		NULL_CHECK(entityp->model->transforms);
		NULL_CHECK(entityp->model->transforms->head);
		NULL_CHECK(entityp->model->transforms->head->transform);
		NULL_CHECK(entityp->model->transforms->neck);
		NULL_CHECK(entityp->model->transforms->neck->transform);
		NULL_CHECK(camera.load());

		if (s_sleepercheck && entity.sleeping)
			continue;

		auto entity_head = get_position(utils::game::get_head_transform(&entity)); /* entity->model->transforms->head NOT head->transform */

		if (entity_head.empty())
			continue;

		auto entity_neck = get_position(utils::game::get_neck_transform(&entity));
		
		if (entity_neck.empty())
			continue;
		
		entity_neck.y -= 1.5f;

		geo::vec2_t screenh;
		geo::vec2_t screenn;
		if (utils::render::world_to_screen(camera, entity_head, &screenh) && utils::render::world_to_screen(camera, entity_neck, &screenn))
		{
			// todo: distance

			int health = (int)entity.health;

			std::string finaltext = "";
			if (s_name)
			{
				std::string name = utils::mono::to_string(entity.display_name);
				finaltext += name + "\n";
			}
			if (s_health)
			{
				char* num = ToChar(health);
				finaltext += std::string(ToChar(health)) + "\n";
				free(num);
			}
			if (s_distance)
			{
				//char* num = ToChar(health);
				//finaltext += std::string(ToChar((int)abs(screenh.y - screenn.y))) + "\n";
			}

			//DrawBox(screenh.x - 10, screenh.y - 10, 20, 20, 1, 255, 0, 0, 255);
			float height = abs(screenh.y - screenn.y);
			float width = height * 0.65;

			if (s_box)
			{
				DrawBox(screenh.x - (width / 2), screenh.y, width, height, 1, 255, 0, 0, 255);
			}

			DrawString(finaltext.c_str(), screenh.x - (width / 2), screenn.y + 15, 255, 0, 0, pFont);
		}

		if (s_aim)
		{
			NULL_CHECK(local_player);
			NULL_CHECK(local_player->input);
			NULL_CHECK(local_player->model);
			NULL_CHECK(local_player->model->transforms);
			NULL_CHECK(local_player->model->transforms->head);
			NULL_CHECK(local_player->model->transforms->head->transform);

			base_player llocal_player = read<base_player>((uint64_t)local_player);
			player_input lplayer_input = read<player_input>((uint64_t)llocal_player.input);
			
			const auto local_head = get_position(utils::game::get_head_transform(&llocal_player));
			
			auto anglecalc = utils::math::calculate_angle(local_head, entity_head);
			const auto calculated_fov = utils::math::calculate_fov(lplayer_input.body_angles, anglecalc);

			if (calculated_fov < aimfov)
			{
				aimfov = calculated_fov;
				aiment = entityp;
			}
		}
	}

	if (s_aim && (GetAsyncKeyState(AIM_KEY) & 0x8000))
	{
		NULL_CHECK_RET(local_player);
		NULL_CHECK_RET(local_player->input);
		NULL_CHECK_RET(local_player->model);
		NULL_CHECK_RET(local_player->model->transforms);
		NULL_CHECK_RET(local_player->model->transforms->head);
		NULL_CHECK_RET(local_player->model->transforms->head->transform);
		
		// entity->model->transforms->head;
		NULL_CHECK_RET(aiment);
		NULL_CHECK_RET(aiment->model);
		NULL_CHECK_RET(aiment->model->transforms);
		NULL_CHECK_RET(aiment->model->transforms->head);
		NULL_CHECK_RET(aiment->model->transforms->head->transform);

		base_player llocal_player = read<base_player>((uint64_t)local_player);
		base_player aaim_ent = read<base_player>((uint64_t)aiment);

		//unity_transform local_tranform = *local_player->model->transforms->head;
		const auto local_head = get_position(utils::game::get_head_transform(&llocal_player));
		
		//unity_transform head_tranform = *aiment->model->transforms->head;
		const auto entity_head = get_position(utils::game::get_head_transform(&aaim_ent));
		
		auto anglecalc = utils::math::calculate_angle(local_head, entity_head);

		// check angles
		if (abs(anglecalc.x) <= 360.0f && abs(anglecalc.y) <= 360.0f && abs(anglecalc.z) <= 360.0f) 
		{
			write<geo::vec3_t>((uint64_t)&llocal_player.input->body_angles, anglecalc);
		}
		//local_player->input->body_angles = anglecalc;
	}
}

void render()
{
	d3ddev->Clear(0, NULL, D3DCLEAR_TARGET, D3DCOLOR_ARGB(0, 0, 255, 0), 1.0f, 0);

	d3ddev->BeginScene();

	__try {
		render_static();
		render_menu(d3ddev);
		render_esp();
	}
	__except (filterException(GetExceptionCode(), GetExceptionInformation())) {
		printf(xorstr_("[-] Error in %s\n"), __FUNCTION__);
	}

	d3ddev->EndScene();
	d3ddev->Present(NULL, NULL, NULL, NULL);
}

LRESULT CALLBACK WindowProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
	switch (message)
	{
	case WM_PAINT:
	{
		DwmExtendFrameIntoClientArea(hWnd, &margin);
	}break;

	case WM_DESTROY:
	{
		PostQuitMessage(0);
		return 0;
	} break;
	}

	return DefWindowProc(hWnd, message, wParam, lParam);
}

void __stdcall RunOverlay()
{	
	printf(xorstr_("[+] Overlay thread started\n"));

	printf(xorstr_("[>] Initializing menu...\n"));
	init_menu();
	printf(xorstr_("[+] Menu initialited\n"));

	HINSTANCE hInstance = GetModuleHandle(NULL);
	RECT rc;
	HWND newhwnd = FindWindow(GAME_WINDOW_CLASS, GAME_WINDOW);
	if (newhwnd != NULL)
	{
		GetWindowRect(newhwnd, &rc);
		width = rc.right - rc.left;
		height = rc.bottom - rc.top;
	}
	else
	{
		printf(xorstr_("[-] Error getting hwnd\n"));
	}

	WNDCLASSEX wc;

	ZeroMemory(&wc, sizeof(WNDCLASSEX));

	wc.cbSize = sizeof(WNDCLASSEX);
	wc.style = CS_HREDRAW | CS_VREDRAW;
	wc.lpfnWndProc = WindowProc;
	wc.hInstance = hInstance;
	wc.hCursor = LoadCursor(NULL, IDC_ARROW);
	wc.hbrBackground = (HBRUSH)RGB(0, 255, 0);
	wc.lpszClassName = "WindowClass";

	RegisterClassEx(&wc);

	hWnd = CreateWindowEx(0,
		"WindowClass",
		"",
		WS_EX_TOPMOST | WS_POPUP,
		rc.left, rc.top,
		width, height,
		NULL,
		NULL,
		hInstance,
		NULL);

	SetWindowLong(hWnd, GWL_EXSTYLE, (int)GetWindowLong(hWnd, GWL_EXSTYLE) | WS_EX_LAYERED | WS_EX_TRANSPARENT);
	SetLayeredWindowAttributes(hWnd, RGB(0, 255, 0), 0, ULW_COLORKEY);
	//SetLayeredWindowAttributes(hWnd, 0, 255, LWA_ALPHA);

	ShowWindow(hWnd, SW_SHOW);

	initD3D(hWnd);

	if (!TEST_BUILD)
	{
		printf(xorstr_("[>] Hiding console window...\n"));
		ShowWindow(GetConsoleWindow(), SW_HIDE);
		printf(xorstr_("[+] Console window hidden\n"));
	}

	MSG msg;
	::SetWindowPos(FindWindow(GAME_WINDOW_CLASS, GAME_WINDOW), HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
	while (!should_exit)
	{
		::SetWindowPos(hWnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);

		if (!FindWindow(GAME_WINDOW_CLASS, GAME_WINDOW))
			printf(xorstr_("[-] Game window is closed\n"));
		// Main render loop
		render();
		// Fps limiter
		if (s_fpslimiter)
		{
			DWORD currentTime = timeGetTime();
			if ((currentTime - LastFrameTime) < (1000 / FPSLimit))
			{
				Sleep(currentTime - LastFrameTime);
			}
			LastFrameTime = currentTime;
			Sleep(1);
		}
		while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE))
		{
			TranslateMessage(&msg);
			DispatchMessage(&msg);
		}

		if (msg.message == WM_QUIT)
			printf(xorstr_("[-] Quit message received\n"));
	}

	CloseWindow(hWnd);
}

void __stdcall entity_loop_thread(void* base_networkable)
{
	while (!should_exit)
	{
		//const auto unk1 = *reinterpret_cast<void**>(std::uintptr_t(base_networkable) + 0xb8);
		const auto unk1 = read<void**>(std::uintptr_t(base_networkable) + 0xb8);

		if (!unk1)
			continue;

		//const auto client_entities = *reinterpret_cast<entity_realm**>(unk1);
		const entity_realm* client_entities = read<entity_realm*>((uint64_t)unk1);

		if (!client_entities)
			continue;

		entity_realm er = read<entity_realm>((uint64_t)client_entities);
		dictionary dic = read<dictionary>((uint64_t)er.list);
		//const auto list = client_entities->list->values;
		const auto listp = dic.values;

		if (!listp)
			continue;

		entity_mutex.lock();

		if (entities.size() >= 500)
			entities.clear();

		entity_mutex.unlock();

		buffer_list bl = read<buffer_list>((uint64_t)listp);
		for (auto i = 0u; i < bl.size; i++)
		{
			const auto element = read<void**>(std::uintptr_t(bl.buffer) + (0x20 + (i * 8)));

			if (!element || std::strstr(utils::mono::get_class_name(element), xorstr_("BasePlayer")) == nullptr)
				continue;

			const auto base_mono_object = read<void**>(std::uintptr_t(element) + 0x10);

			if (!base_mono_object)
				continue;

			const auto object = read<void**>(std::uintptr_t(base_mono_object) + 0x30);

			if (!object)
				continue;

			//const auto object_1 = *reinterpret_cast<game_object**>(std::uintptr_t(object) + 0x30);
			const game_object* gmp = read<game_object*>(std::uintptr_t(object) + 0x30);
			game_object gm = read<game_object>((uint64_t)gmp);

			//if (!object_1)
			//	continue;

			unk2 u2 = read<unk2>((uint64_t)gm.unk);
			base_player player = read<base_player>((uint64_t)u2.player);
			//const auto player = object_1->unk->player;

			std::lock_guard guard(entity_mutex);

			if (player.health <= 0.8f || std::find(entities.begin(), entities.end(), u2.player) != entities.end())
				continue;

			entities.push_back(u2.player);
		}

		std::this_thread::sleep_for(std::chrono::seconds(10));
	}
}

void __stdcall camera_loop_thread( void* game_object_manager )
{
	while ( !should_exit )
	{
		//const auto last_object = *reinterpret_cast< unk1** >( game_object_manager );
		unk1* last_objectp = read<unk1*>((uint64_t)game_object_manager);
		unk1 last_object = read<unk1>((uint64_t)last_objectp);
		//const auto first_object = *reinterpret_cast< unk1** >( std::uintptr_t( game_object_manager ) + 0x8 );
		unk1* first_objectp = read<unk1*>(std::uintptr_t(game_object_manager) + 0x8);
		unk1 first_object = read<unk1>((uint64_t)first_objectp);

		for ( auto object = first_objectp; object != last_objectp; object = object->next )
		{
			unk1 objectl = read<unk1>((uint64_t)object);
			mono_object mo = read<mono_object>((uint64_t)objectl.object);
			if (mo.tag == 5 )
			{
				camera.store( reinterpret_cast< base_camera* >( object->object->object->unk ) );
				break;
			}
		}

		std::this_thread::sleep_for( std::chrono::seconds( 20 ) );
	}
}

DWORD GetMainThread(DWORD dwProcID)
{
	DWORD dwMainThreadID = 0;
	ULONGLONG ullMinCreateTime = MAXULONGLONG;

	HANDLE hThreadSnap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
	if (hThreadSnap != INVALID_HANDLE_VALUE) {
		THREADENTRY32 th32;
		th32.dwSize = sizeof(THREADENTRY32);
		BOOL bOK = TRUE;
		for (bOK = Thread32First(hThreadSnap, &th32); bOK;
			bOK = Thread32Next(hThreadSnap, &th32)) {
			if (th32.th32OwnerProcessID == dwProcID) {
				HANDLE hThread = OpenThread(THREAD_QUERY_INFORMATION,
					TRUE, th32.th32ThreadID);
				if (hThread) {
					FILETIME afTimes[4] = { 0 };
					if (GetThreadTimes(hThread,
						&afTimes[0], &afTimes[1], &afTimes[2], &afTimes[3])) {
						ULONGLONG ullTest = (ULONGLONG)(afTimes[0].dwLowDateTime,
							afTimes[0].dwHighDateTime);
						if (ullTest && ullTest < ullMinCreateTime) {
							ullMinCreateTime = ullTest;
							dwMainThreadID = th32.th32ThreadID;
						}
					}
					CloseHandle(hThread);
				}
			}
		}
		CloseHandle(hThreadSnap);
	}
	return dwMainThreadID;
}

std::vector<DWORD> threadlist;
int number = 0;
void HijackThread(DWORD64 func, int id) 
{
	printf(xorstr_("[>] Hijacking thread for func %p...\n"), func);

	THREADENTRY32 te32;
	THREADENTRY32 tte32;
	te32.dwSize = sizeof(THREADENTRY32);
	CONTEXT ctx;
	ctx.ContextFlags = CONTEXT_FULL;

	HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
	Thread32First(hSnap, &te32);

	printf(xorstr_("[+] TH32 handle %p\n"), hSnap);
	DWORD mainthread = GetMainThread(GetCurrentProcessId());
	printf(xorstr_("[+] Main thread id %d\n"), mainthread);

	if (threadlist.empty()) 
	{
		while (Thread32Next(hSnap, &te32))
		{
			if (GetCurrentThreadId() == te32.th32ThreadID)
			{
				printf(xorstr_("[-] Thread is current thread\n"));
				continue;
			}
			// Check if the thread was not already hijacked
			if (std::find(threadlist.begin(), threadlist.end(), te32.th32ThreadID) != threadlist.end())
			{
				printf(xorstr_("[-] Thread already hijacked\n"));
				continue;
			}
			if (GetCurrentThreadId() == mainthread)
			{
				printf(xorstr_("[-] Skipped main thread\n"));
				continue;
			}
			if (GetCurrentProcessId() == te32.th32OwnerProcessID)
			{
				/*if (number == 0)
				{
					// We don't want main thread
					number++;
					printf(xorstr_("[-] Skipped main thread\n"));
					continue;
				}*/
				threadlist.push_back(te32.th32ThreadID);
				//printf(xorstr_("[+] Found thread with id %d\n"), te32.th32ThreadID);
			}
		}
		printf(xorstr_("[+] Found %i threads\n"), threadlist.size());
	}
	CloseHandle(hSnap);

	int threadpos = id;
	DWORD threadid = 0;;
	while (threadid == 0) 
	{
		threadid = threadlist[threadlist.size() - threadpos];
		threadpos++;
	}
	printf(xorstr_("[+] Using thread with id %d\n"), threadid);

	HANDLE hThread = OpenThread(THREAD_ALL_ACCESS, FALSE, threadid);

	if (!hThread)
	{
		printf(xorstr_("[-] Unable to open target thread handle (%d)\n"), GetLastError());
	}

	SuspendThread(hThread);
	GetThreadContext(hThread, &ctx);

	ctx.Rip = (DWORD64)func;

	if (!SetThreadContext(hThread, &ctx))
	{
		printf(xorstr_("[-] Unable to set thread context (%d)\n"), GetLastError());
	}

	ResumeThread(hThread);
	CloseHandle(hThread);

	printf(xorstr_("[+] Context set\n"));
}

void __stdcall main_thread()
{	
	printf(xorstr_("[+] Main thread started\n"));

	bool handleo = open();
	if (!handleo) 
	{
		printf("[-] Failed to open process handle");
	}
	
	const auto base_networkable_address = utils::memory::find_signature("GameAssembly.dll", "48 8b 05 ? ? ? ? 48 8b 88 ? ? ? ? 48 8b 09 48 85 c9 74 ? 45 33 c0 8b" );

	if ( !base_networkable_address )
		return;

	const auto base_networkable = reinterpret_cast<std::uintptr_t>( base_networkable_address + *reinterpret_cast< std::int32_t* >( base_networkable_address + 3 ) + 7 );

	if ( !base_networkable )
		return;

	std::printf(xorstr_("[+] BaseNetworkable: 0x%llx\n"), ( base_networkable - std::uintptr_t( GetModuleHandleA( "GameAssembly.dll" ) ) ) );

	std::thread entity_iteration(&entity_loop_thread, *reinterpret_cast<void**>(base_networkable));

	const auto game_object_manager_address = utils::memory::find_signature( "UnityPlayer.dll", "48 89 05 ? ? ? ? 48 83 c4 38 c3 48 c7 05 ? ? ? ? ? ? ? ? 48 83 c4 38 c3 cc cc cc cc cc 48" );

	if ( !game_object_manager_address )
		return;

	const auto game_object_manager = reinterpret_cast< std::uintptr_t >( game_object_manager_address + *reinterpret_cast< std::int32_t* >( game_object_manager_address + 3 ) + 7 );

	if ( !game_object_manager )
		return;

	std::printf(xorstr_("[+] GameObjectManager: 0x%llx\n"), ( game_object_manager - std::uintptr_t( GetModuleHandleA( "UnityPlayer.dll" ) ) ) );

	std::thread etc_iteration( &camera_loop_thread, *reinterpret_cast< void** >( game_object_manager ) );	

	while ( true )
	{
		//std::lock_guard guard( entity_mutex );

		std::this_thread::sleep_for(std::chrono::milliseconds(50));

		if (entities.empty())
			continue;

		local_player = entities.front();

		/*for ( const auto& entity : entities )
		{
			if ( !entity )
				continue;

			NULL_CHECK(entity->player_model);

			if (utils::game::is_local_player(entity->player_model))
			{
				local_player = entity;
			}
		}*/
	}

	/*should_exit = true;
	entity_iteration.join( );
	etc_iteration.join( );

	fclose( reinterpret_cast< FILE* >( stdin ) );
	fclose( reinterpret_cast< FILE* >( stdout ) );
	FreeConsole( );
	PostMessage( GetConsoleWindow( ), WM_CLOSE, 0, 0 );*/
}

void __stdcall Init() 
{
	AllocConsole();
	freopen_s(reinterpret_cast<FILE**>(stdin), "CONIN$", "r", stdin);
	freopen_s(reinterpret_cast<FILE**>(stdout), "CONOUT$", "w", stdout);

	printf(xorstr_("\n\ttsur\n"));
	printf(xorstr_("\tCopyright (c) xcheats.cc - All rights reserved\n\n"));

	printf(xorstr_("[>] Hijacking threads...\n"));
	HijackThread((DWORD64)main_thread, TARGET_THREAD);
	HijackThread((DWORD64)RunOverlay, TARGET_THREAD + 1);
}

bool __stdcall DllMain( HMODULE module, std::uint32_t call_reason, void* )
{
	if ( call_reason != DLL_PROCESS_ATTACH )
		return false;

	if (TEST_BUILD) 
	{
		AllocConsole();
		freopen_s(reinterpret_cast<FILE**>(stdin), "CONIN$", "r", stdin);
		freopen_s(reinterpret_cast<FILE**>(stdout), "CONOUT$", "w", stdout);
		
		if ( const auto handle = CreateThread( nullptr, 0, reinterpret_cast< LPTHREAD_START_ROUTINE >( main_thread ), module, 0, nullptr ); handle != NULL )
			CloseHandle( handle );

		if (const auto handle = CreateThread(nullptr, 0, reinterpret_cast<LPTHREAD_START_ROUTINE>(RunOverlay), module, 0, nullptr); handle != NULL)
			CloseHandle(handle);
	}
	else 
	{
		Init();
	}
	
	return true;
}
