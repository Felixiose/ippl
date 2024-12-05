import os
import subprocess
import numpy as np

def calculate_velocity_scaling(total_particles_initial, total_particles_scaled, velocity_initial):
    """Scale velocities to keep the total energy constant."""
    scaling_factor = (total_particles_initial / total_particles_scaled) ** (1/3)
    return velocity_initial * scaling_factor

def modify_initial_conditions(input_file, output_file, scale_factor):
    """Adjust particle velocities in the initial conditions file."""
    with open(input_file, 'r') as infile, open(output_file, 'w') as outfile:
        for line in infile:
            values = list(map(float, line.strip().split(',')))
            # Scale velocities (assuming they are in columns 3 to 5)
            values[3] *= scale_factor
            values[4] *= scale_factor
            values[5] *= scale_factor
            outfile.write(','.join(map(str, values)) + '\n')

def run_simulation(ic_folder, grid_size, num_particles, num_steps, solver, lb_threshold, step_method, output_file):
    """Run the StructureFormation simulation."""
    command = [
        "mpirun", "-np", str(num_processes),
        "./StructureFormation",
        ic_folder, *map(str, grid_size), str(num_particles),
        str(num_steps), solver, str(lb_threshold), step_method,
        "--overallocate", "1.0", "--info", "10"
    ]
    with open(output_file, 'w') as log_file:
        subprocess.run(command, stdout=log_file, stderr=log_file)

# Parameters
initial_particles = 32768
velocity_initial = 1.0  # Example initial velocity scaling factor
initial_conditions_file = "data/Data.csv"
ic_folder = "data/"
grid_size = [32, 32, 32]
num_steps = 10
solver = "FFT"
lb_threshold = 1.0
step_method = "LeapFrog"
num_processes_list = [1, 2, 4, 8, 16]  # Example process counts

# Weak Scaling
for num_processes in num_processes_list:
    particles_scaled = initial_particles * num_processes
    velocity_scale = calculate_velocity_scaling(initial_particles, particles_scaled, velocity_initial)
    scaled_ic_file = f"{ic_folder}/Data_{particles_scaled}.csv"
    modify_initial_conditions(initial_conditions_file, scaled_ic_file, velocity_scale)
    
    output_file = f"weak_scaling_{num_processes}_processes.log"
    run_simulation(ic_folder, grid_size, particles_scaled, num_steps, solver, lb_threshold, step_method, output_file)

# Strong Scaling
particles_fixed = initial_particles
velocity_scale = 1.0  # No change for strong scaling
scaled_ic_file = f"{ic_folder}/Data_fixed.csv"
modify_initial_conditions(initial_conditions_file, scaled_ic_file, velocity_scale)

for num_processes in num_processes_list:
    output_file = f"strong_scaling_{num_processes}_processes.log"
    run_simulation(ic_folder, grid_size, particles_fixed, num_steps, solver, lb_threshold, step_method, output_file)
