# ALA-GA projects

Research projects for the NSTF (Robotics and Intelligent Machines / Science Innovation Expo), Region V – Bicol.

## KALINGA (root of this repository)

A low-cost, 3D-printed, Baymax-inspired touch-screen health pendant with GPS, 4G calling, vital-sign and symptom monitoring, fall/SOS/safe-zone alerts, a timed pill carousel, and the KALINGA Monitor family app.

- `index.html` – the KALINGA website: features, install button, how to install, downloads
- `app/` – the KALINGA Monitor app (installable web app; **Try the demo** works without the pendant)
- `firmware/kalinga_pendant/` – pendant program (Waveshare ESP32-S3 1.28-in round touch LCD)
- `cad/` – 3D print files (`front_shell.stl`, `back_shell.stl`, `pill_carousel.stl`) and the script that makes them
- `RIM4_KALINGA_NSTF-SIE_Manuscript.*` – NSTF Science Innovation Expo manuscript
- `RIM4_KALINGA_PR2_Presentation.pptx`, `RIM4_KALINGA_Engineering_Notebook.pdf`, `RIM4_KALINGA_Rationale_Script.pdf`

## ALAGA (`ALAGA/`)

A low-cost wheeled soft inflatable companion robot for older adults home alone: NSTF RIM manuscript, PR2 deck, build guide, robot and pendant programs.

## Publish the website and app

Installing the app needs HTTPS. With GitHub Pages: **Settings → Pages → Build and deployment → Deploy from a branch → `main` and `/ (root)` → Save.**

- Website: https://pogi6969.github.io/ALA-GA-/
- App: https://pogi6969.github.io/ALA-GA-/app/

Test locally with `python3 -m http.server 8000`, then open http://localhost:8000. After changing the app, bump `VERSION` in `sw.js` and `app/sw.js` so installed copies refresh.
