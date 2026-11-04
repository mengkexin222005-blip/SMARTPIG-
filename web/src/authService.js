import {
  onAuthStateChanged,
  signInWithEmailAndPassword,
  createUserWithEmailAndPassword,
  signOut,
  sendPasswordResetEmail,
  EmailAuthProvider,
  reauthenticateWithCredential,
  updatePassword,
  setPersistence,
  browserLocalPersistence,
  browserSessionPersistence,
} from "firebase/auth";
import { auth } from "./firebase";
import { recordSignIn } from "./dataService";

export function subscribeToAuth(onChange) {
  return onAuthStateChanged(auth, onChange);
}

// remember = true: stay signed in after the browser closes.
// remember = false: signed out when the browser (tab session) closes.
export async function signIn(email, password, remember = true) {
  await setPersistence(auth, remember ? browserLocalPersistence : browserSessionPersistence);
  const cred = await signInWithEmailAndPassword(auth, email.trim(), password);
  // Save who signed in. A failure here must not block the sign-in.
  recordSignIn(cred.user).catch((e) => console.warn("Could not save sign-in info:", e));
  return cred;
}

// Anyone can make an account. Their name, email and sign-in info are saved in /users.
export async function signUp(name, email, password, remember = true) {
  await setPersistence(auth, remember ? browserLocalPersistence : browserSessionPersistence);
  const cred = await createUserWithEmailAndPassword(auth, email.trim(), password);
  await recordSignIn(cred.user, name).catch((e) => console.warn("Could not save profile:", e));
  return cred;
}

export function signOutUser() {
  return signOut(auth);
}

export function resetPassword(email) {
  return sendPasswordResetEmail(auth, email.trim());
}

// Firebase asks for the current password again before changing it
export async function changePassword(currentPassword, newPassword) {
  const user = auth.currentUser;
  if (!user || !user.email) throw new Error("Not signed in");
  const cred = EmailAuthProvider.credential(user.email, currentPassword);
  await reauthenticateWithCredential(user, cred);
  await updatePassword(user, newPassword);
}

// Firebase error code -> translation key (see i18n.js "auth.*")
export function authErrorKey(error) {
  const code = error && error.code ? error.code : "";
  switch (code) {
    case "auth/invalid-email":
      return "auth.invalidEmail";
    case "auth/invalid-credential":
    case "auth/invalid-login-credentials":
    case "auth/wrong-password":
    case "auth/user-not-found":
      return "auth.wrong";
    case "auth/too-many-requests":
      return "auth.tooMany";
    case "auth/network-request-failed":
      return "auth.network";
    case "auth/email-already-in-use":
      return "auth.inUse";
    case "auth/weak-password":
      return "auth.weak";
    case "auth/operation-not-allowed":
      return "auth.notAllowed";
    case "auth/requires-recent-login":
      return "auth.recent";
    default:
      return "auth.other";
  }
}
