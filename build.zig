const std = @import("std");

pub fn build(b: *std.Build) void {
    const target = b.resolveTargetQuery(.{
        .cpu_arch = .x86,
        .os_tag = .windows,
        .abi = .msvc,
    });
    const optimize = b.standardOptimizeOption(.{});

    const hook_mod = b.dependency("hook", .{
        .target = target,
        .optimize = optimize,
    }).module("hook");

    const lib = b.addLibrary(.{
        .name = "transmogfix",
        .linkage = .dynamic,
        .root_module = b.createModule(.{
            .root_source_file = b.path("src/main.zig"),
            .target = target,
            .optimize = optimize,
            .imports = &.{
                .{ .name = "hook", .module = hook_mod },
            },
        }),
    });

    b.installArtifact(lib);
}
