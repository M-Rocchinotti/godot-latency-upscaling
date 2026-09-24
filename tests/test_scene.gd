extends RefCounted
## The procedural, static test scene of the upscaler and DLSS image tests: a checkerboard floor, thin
## bars and spheres (fine detail an upscaler has to resolve), linear tonemapping, no animation.


## Builds the scene under `parent` and returns its current camera.
static func build(parent: Node) -> Camera3D:
	var world := Node3D.new()
	parent.add_child(world)
	var env := WorldEnvironment.new()
	var environment := Environment.new()
	environment.background_mode = Environment.BG_COLOR
	environment.background_color = Color(0.45, 0.55, 0.7)
	environment.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	environment.ambient_light_color = Color(0.5, 0.5, 0.5)
	environment.tonemap_mode = Environment.TONE_MAPPER_LINEAR
	env.environment = environment
	world.add_child(env)
	var sun := DirectionalLight3D.new()
	sun.rotation_degrees = Vector3(-50, 30, 0)
	world.add_child(sun)

	var checker := Image.create_empty(512, 512, true, Image.FORMAT_RGB8)
	for y in 512:
		for x in 512:
			var on := ((x / 8) + (y / 8)) % 2 == 0
			checker.set_pixel(x, y, Color(0.9, 0.9, 0.9) if on else Color(0.1, 0.12, 0.15))
	checker.generate_mipmaps()
	var floor_material := StandardMaterial3D.new()
	floor_material.albedo_texture = ImageTexture.create_from_image(checker)
	floor_material.uv1_scale = Vector3(4, 4, 1)
	var floor_mesh := MeshInstance3D.new()
	var plane := PlaneMesh.new()
	plane.size = Vector2(40, 40)
	floor_mesh.mesh = plane
	floor_mesh.material_override = floor_material
	world.add_child(floor_mesh)

	var bar_material := StandardMaterial3D.new()
	bar_material.albedo_color = Color(0.95, 0.35, 0.1)
	for i in 24:
		var bar := MeshInstance3D.new()
		var box := BoxMesh.new()
		box.size = Vector3(0.03, 3.0, 0.03)
		bar.mesh = box
		bar.material_override = bar_material
		bar.position = Vector3(-6.0 + i * 0.5, 1.5, -2.0 - (i % 5) * 1.3)
		world.add_child(bar)
	var sphere_material := StandardMaterial3D.new()
	sphere_material.albedo_color = Color(0.2, 0.6, 0.9)
	for i in 5:
		var sphere := MeshInstance3D.new()
		var mesh := SphereMesh.new()
		mesh.radius = 0.5
		mesh.height = 1.0
		sphere.mesh = mesh
		sphere.material_override = sphere_material
		sphere.position = Vector3(-3.0 + i * 1.5, 0.5, 1.0)
		world.add_child(sphere)

	var camera := Camera3D.new()
	camera.fov = 60.0
	world.add_child(camera)
	reset_camera(camera)
	camera.make_current()
	return camera


static func reset_camera(camera: Camera3D) -> void:
	camera.position = Vector3(0, 1.6, 6)
	camera.rotation = Vector3(deg_to_rad(-12), 0, 0)
