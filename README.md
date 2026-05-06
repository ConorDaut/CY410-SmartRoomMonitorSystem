# CY410-SmartRoomMonitorSystem
This repository contains the code for the Smart Room Monitor System, a group project for CY410 Spring 2026. The code was generated using Claude.
- Group Members: Jalal Akhoun, Conor Daut, Lauryn Holloway, Caleb Palmer, Dawson Westnedge

Initial Claude Prompt: "I am going to provide to you a pdf and a zip file. I want you to use the functional requirements outlined in the pdf file, and use them to create a smart room monitor system. Please use the zip file contents as reference and as a template (basically, try to structure it similar to how the zip file has it structured). Please all files for the code in full, and note that this will be deployed on an ESP32, with a ov3660 camera."
- Some debugging had to be performed afterwords

Security Issue: During our project presentation, we identified and proposed a solution to a security issue in the project related to the image handler. By using /image?f=<filename>, any user in the system can access any image, regardless of who the owner is.
- The reason for this security issue is the image handler only checks if there is a session token, and does not verify the username
- To fix this, you would have to implement an if else statement to check the username associated with the image being accessed.
- This can be found starting at line 875 in the app_httpd.cpp file.
