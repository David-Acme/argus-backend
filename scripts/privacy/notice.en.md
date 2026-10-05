ARGUS PRIVACY NOTICE AND TERMS OF USE (version {{VERSION}})

Read this before installing. Argus is not installed or configured until you
accept it.

1. Where your data stays
   Argus runs entirely on this computer. Cameras, faces, voices, the agenda
   and notifications are processed and stored here, on your hardware. Nothing
   is sent to the cloud. If you turn on remote access, the tunnel only carries
   encrypted data between your devices and this computer.

2. What Argus processes
   - Video and, when the camera has a microphone, audio from the cameras you
     add.
   - Faces: to sign in (each person registers their own) and, only if that
     person allows it, to recognize them on the cameras.
   - Voice: calls with Argus are transcribed on this computer. Argus learns to
     recognize a person's voice only if they allow it.
   - Presence: if each person allows it, Argus works out whether they are home
     (from their connection to the home network or an entrance camera) to
     decide whom to warn first during an alarm. It keeps only the current state
     (home / away), never a location.
   - Notifications, alert calls, the household agenda and projects.

3. Sensitive data
   Faces and voices are biometric data, which {{LAWS}} treats as sensitive
   data. Argus stores them as numeric patterns, not as recordings. The photo
   each person takes when registering is kept in this computer's private
   storage and is never synced to phones.

4. How long data is kept
   - Argus does not record video continuously. Images of security events are
     kept for review and deleted automatically.
   - Faces of strangers passing the cameras: 30 days without being seen again.
   - Learned voices: pending samples 30 days; confirmed samples 180 days;
     everything is erased as soon as the person withdraws permission.
   - Presence: only the current state; erased when permission is withdrawn or
     after 30 days without change.
   - In {{COUNTRY}}, the rules ask that video surveillance recordings be kept
     {{VIDEO_DAYS}} days (at most {{VIDEO_MAX_DAYS}}), and up to
     {{INCIDENT_DAYS}} days when they show a possible incident or offence.
     Check that your installation's retention settings respect those periods.

5. Each person decides
   The first time they open the app, each person chooses what they allow:
   presence, face recognition on cameras, voice learning and camera audio.
   They can change it at any time in Profile > Privacy. Without their
   permission, those features stay off for them. As the owner you can turn a
   feature off for the whole household, but never turn it on for someone
   else.

6. Your responsibilities
   - Comply with your country's laws on video surveillance and third-party
     data. In {{COUNTRY}}: {{LAWS}}, overseen by the {{AUTHORITY}}.
   - Put up visible signs wherever cameras are watching.
   - Do not point cameras at other people's spaces (the street, neighbouring
     homes) more than necessary.
   - Tell the people who live, work in or visit the place.

7. How to withdraw your acceptance
   Stop Argus (cd argus-deploy && docker compose down, without -v) and run
   this same script with --withdraw-privacy-consent. To erase all data,
   delete the installation's data folder. Each person can withdraw their
   permissions from the app at any time.

8. Software in development, no warranty
   - Argus is in development (pre-beta) and is provided "as is", without
     warranties of any kind.
   - Its author is not responsible for misuse or malfunction of the software.
   - Argus is not a certified security or alarm system. It does not replace a
     professional monitoring service or emergency services.
   - You are responsible for complying with local laws on video surveillance
     and third-party data, including camera signage.

This notice is not legal advice.
