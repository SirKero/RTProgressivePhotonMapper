: This script searches for Mogwai builds and starts the most suitable one with a Demo script.

@echo off

if exist build\windows-vs2022-d3d12\bin\Release\Mogwai.exe (
	echo Visual Studio 2022 Release build found!
	build\windows-vs2022-d3d12\bin\Release\Mogwai.exe --script=PhotonMapPasses\DemoCausticGlass.py
) else if exist build\windows-vs2019-d3d12\bin\Release\Mogwai.exe (
	echo Visual Studio 2019 Release build found!
	build\windows-vs2019-d3d12\bin\Release\Mogwai.exe --script=PhotonMapPasses\DemoCausticGlass.py
) else if exist build\windows-vs2022-d3d12\bin\Debug\Mogwai.exe (
	echo Visual Studio 2022 Debug build found! Please consider building in Release configuration for optimal performance!
	build\windows-vs2022-d3d12\bin\Debug\Mogwai.exe --script=PhotonMapPasses\DemoCausticGlass.py
) else if exist build\windows-vs2019-d3d12\bin\Debug\Mogwai.exe (
	echo Visual Studio 2019 Debug build found! Please consider building in Release configuration for optimal performance!
	build\windows-vs2019-d3d12\bin\Debug\Mogwai.exe --script=PhotonMapPasses\DemoCausticGlass.py
) else (
	echo No D3D12 build found. Please make sure to run setup_vs20xx.bat and build the code in Release or Debug configuration beforehand!
	@pause
)