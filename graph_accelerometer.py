import datetime as dt
import matplotlib.pyplot as plt
import matplotlib.animation as animation
import numpy as np
import sys

# Create figure for plotting
fig = plt.figure()
x_plot = fig.add_subplot(3, 1, 1)
y_plot = fig.add_subplot(3, 1, 2)
z_plot = fig.add_subplot(3, 1, 3)

x_axis = []
x_readings = []
y_readings = []
z_readings = []
text_reading = ""

interval = 100

def getReadings(x_readings, y_readings, z_readings):
    data = sys.stdin.readline()

    if(data[0].isalpha()):
        print(data)
        data = "50.00,50.00,50.00"

    parts = data.split(",")

    x_readings.append(float(parts[0]))
    y_readings.append(float(parts[1]))
    z_readings.append(float(parts[2]))


# This function is called periodically from FuncAnimation
def animate(i, x_axis, x_readings, y_readings, z_readings):

    # Add x and y to lists
    getReadings(x_readings, y_readings, z_readings)

    x_axis.append(dt.datetime.now().strftime('%H:%M:%S.%f'))

    # Limit x and y lists to 20 items
    x_axis = x_axis[-20:]
    x_readings = x_readings[-20:]
    y_readings = y_readings[-20:]
    z_readings = z_readings[-20:]

    # Draw x and y lists
    x_plot.clear()
    x_plot.plot(x_axis, x_readings)

    # Draw y and y lists
    y_plot.clear()
    y_plot.plot(x_axis, y_readings)

    # Draw z and y lists
    z_plot.clear()
    z_plot.plot(x_axis, z_readings)

    # Format plot
    plt.xticks(rotation=45, ha='right')
    plt.subplots_adjust(bottom=0.30)
    plt.tight_layout()

    y_plot.set_ylim(bottom=-1000)
    y_plot.set_ylim(top=1000)

    x_plot.set_ylim(bottom=-1000)
    x_plot.set_ylim(top=1000)

    z_plot.set_ylim(bottom=-1000)
    z_plot.set_ylim(top=1000)

    y_plot.set_xticklabels([])
    x_plot.set_xticklabels([])

    x_plot.title.set_text("X-Axis")
    y_plot.title.set_text("Y-Axis")
    z_plot.title.set_text("Z-Axis")

# Set up plot to call animate() function periodically
ani = animation.FuncAnimation(fig, animate, fargs=(x_axis, x_readings, y_readings, z_readings), interval=interval)
plt.show()
