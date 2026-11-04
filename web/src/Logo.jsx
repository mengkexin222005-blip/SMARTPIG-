// SmartPig logo: the piglet peeking over the smart feeding trough.
// Images live in public/brand/ (a small one for icons, a large one for the login).
export function LogoMark({ size = 40, className = "" }) {
  const src = size > 96 ? "/brand/smartpig-logo.png" : "/brand/smartpig-logo-128.png";
  return (
    <img
      className={`logo-mark ${className}`}
      src={src}
      width={size}
      height={size}
      alt=""
      aria-hidden="true"
      decoding="async"
    />
  );
}

export function Wordmark({ size = 22 }) {
  return (
    <span className="wordmark" style={{ fontSize: size }}>
      Smart<span className="wordmark-accent">Pig</span>
    </span>
  );
}

// Large logo for the login panel
export function LoginIllustration() {
  return (
    <img
      className="login-art"
      src="/brand/smartpig-logo.png"
      width={420}
      height={420}
      alt="SmartPig logo: a piglet looking over a smart feeding trough"
    />
  );
}
