-- MAME autoboot script: save a snapshot every SNAP_EVERY frames (default 300)
-- and exit after SNAP_FRAMES frames (default 6000).
local every = tonumber(os.getenv("SNAP_EVERY") or "300")
local total = tonumber(os.getenv("SNAP_FRAMES") or "6000")
local frame = 0
snap_notifier = emu.add_machine_frame_notifier(function()
	frame = frame + 1
	if frame % every == 0 then
		manager.machine.video:snapshot()
	end
	if frame >= total then
		manager.machine:exit()
	end
end)
