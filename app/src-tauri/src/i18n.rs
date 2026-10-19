//! Messages shown to the player (steps, errors, journal) in French, English, Spanish and Portuguese (Latin America).
//! The UI sends the language it uses (`set_language`); English is the fallback.

use std::fmt::Display;
use std::sync::atomic::{AtomicUsize, Ordering};

static LANG: AtomicUsize = AtomicUsize::new(1);

/// fr, en, es, pt (anything else: en).
pub fn set(lang: &str) {
    let i = match lang.get(..2).unwrap_or("").to_ascii_lowercase().as_str() {
        "fr" => 0,
        "es" => 2,
        "pt" => 3,
        _ => 1,
    };
    LANG.store(i, Ordering::Relaxed);
}

/// The message in the current language.
pub fn t(key: &str) -> &'static str {
    let texts = MESSAGES.iter().find(|(k, _)| *k == key).map(|(_, v)| v).unwrap_or(&[""; 4]);
    texts[LANG.load(Ordering::Relaxed)]
}

/// The message with each `{}` replaced by the next argument.
pub fn tf(key: &str, args: &[&dyn Display]) -> String {
    let mut out = String::new();
    let mut args = args.iter();
    let mut parts = t(key).split("{}").peekable();
    while let Some(part) = parts.next() {
        out.push_str(part);
        if parts.peek().is_some() {
            if let Some(a) = args.next() {
                out.push_str(&a.to_string());
            }
        }
    }
    out
}

type Texts = [&'static str; 4];

#[rustfmt::skip]
const MESSAGES: &[(&str, Texts)] = &[
    // launch steps
    ("step_connecting", ["Connexion au serveur…", "Connecting to the server…", "Conectando con el servidor…", "Conectando ao servidor…"]),
    ("step_preparing", ["Préparation du jeu…", "Preparing the game…", "Preparando el juego…", "Preparando o jogo…"]),
    ("step_launching", ["Lancement de World of Warcraft…", "Starting World of Warcraft…", "Iniciando World of Warcraft…", "Iniciando World of Warcraft…"]),
    ("running", ["Jeu lancé : connectez-vous au royaume", "Game started: log in to the realm", "Juego iniciado: entra al reino", "Jogo iniciado: entre no reino"]),
    ("ready", ["Clé du serveur en place : entrez dans le royaume", "Server key in place: enter the realm", "Clave del servidor lista: entra al reino", "Chave do servidor pronta: entre no reino"]),
    ("closed", ["Le jeu est fermé", "The game is closed", "El juego se cerró", "O jogo foi fechado"]),
    ("not_started", ["Le jeu n'a pas démarré (aucun WowB.exe trouvé en 3 minutes)", "The game did not start (no WowB.exe found within 3 minutes)", "El juego no arrancó (no se encontró WowB.exe en 3 minutos)", "O jogo não iniciou (nenhum WowB.exe encontrado em 3 minutos)"]),
    ("mac_copying", ["Copie du jeu pour Forever (première fois ou après une mise à jour)…", "Copying the game for Forever (first time or after an update)…", "Copiando el juego para Forever (primera vez o tras una actualización)…", "Copiando o jogo para o Forever (primeira vez ou após uma atualização)…"]),
    ("mac_signing", ["Signature de la copie du jeu…", "Signing the game copy…", "Firmando la copia del juego…", "Assinando a cópia do jogo…"]),
    // launch errors
    ("already_running", ["le jeu est déjà lancé", "the game is already running", "el juego ya está abierto", "o jogo já está aberto"]),
    ("game_not_found", ["jeu introuvable : choisissez le dossier _classic_beta_ dans les paramètres", "game not found: choose the _classic_beta_ folder in the settings", "juego no encontrado: elige la carpeta _classic_beta_ en los ajustes", "jogo não encontrado: escolha a pasta _classic_beta_ nas configurações"]),
    ("login_first", ["connectez-vous d'abord", "log in first", "inicia sesión primero", "entre na sua conta primeiro"]),
    ("session_expired", ["session expirée : reconnectez-vous", "session expired: log in again", "sesión caducada: vuelve a iniciar sesión", "sessão expirada: entre novamente"]),
    ("session_save", ["enregistrement de la session : {}", "saving the session: {}", "guardando la sesión: {}", "salvando a sessão: {}"]),
    ("launch_failed", ["lancement de {} : {}", "starting {}: {}", "iniciando {}: {}", "iniciando {}: {}"]),
    ("wtf_folder", ["dossier WTF : {}", "WTF folder: {}", "carpeta WTF: {}", "pasta WTF: {}"]),
    ("registry", ["registre : {}", "registry: {}", "registro: {}", "registro: {}"]),
    ("mac_failed", ["{} a échoué : {}", "{} failed: {}", "{} falló: {}", "{} falhou: {}"]),
    ("mac_not_started", ["le jeu n'a pas pu démarrer (open : {})", "the game could not start (open: {})", "el juego no pudo arrancar (open: {})", "o jogo não pôde iniciar (open: {})"]),
    ("mac_module", ["le module Forever ne s'est pas chargé dans le jeu", "the Forever module was not loaded into the game", "el módulo Forever no se cargó en el juego", "o módulo Forever não foi carregado no jogo"]),
    ("mac_key", ["dérivation de la clé du ticket impossible", "could not derive the ticket key", "no se pudo derivar la clave del ticket", "não foi possível derivar a chave do ticket"]),
    ("mac_encrypt", ["chiffrement du ticket impossible ({})", "could not encrypt the ticket ({})", "no se pudo cifrar el ticket ({})", "não foi possível criptografar o ticket ({})"]),
    ("mac_user", ["nom d'utilisateur inconnu", "unknown user name", "nombre de usuario desconocido", "nome de usuário desconhecido"]),
    // server API
    ("address_not_found", ["adresse du serveur introuvable ({})", "server address not found ({})", "dirección del servidor no encontrada ({})", "endereço do servidor não encontrado ({})"]),
    ("unexpected_answer", ["réponse inattendue du serveur", "unexpected answer from the server", "respuesta inesperada del servidor", "resposta inesperada do servidor"]),
    ("authenticator", ["ce compte utilise un authentificateur, non géré par le launcher", "this account uses an authenticator, which the launcher does not support", "esta cuenta usa un autenticador, que el launcher no admite", "esta conta usa um autenticador, que o launcher não suporta"]),
    ("bad_login", ["e-mail ou mot de passe incorrect", "wrong e-mail or password", "correo o contraseña incorrectos", "e-mail ou senha incorretos"]),
    ("remember_refused", ["le serveur refuse la connexion automatique (HTTP {})", "the server refuses automatic login (HTTP {})", "el servidor rechaza el inicio de sesión automático (HTTP {})", "o servidor recusa o login automático (HTTP {})"]),
    ("banned", ["compte banni", "account banned", "cuenta suspendida", "conta banida"]),
    ("server_error", ["erreur du serveur (HTTP {})", "server error (HTTP {})", "error del servidor (HTTP {})", "erro do servidor (HTTP {})"]),
    ("unreachable", ["serveur injoignable", "server unreachable", "servidor inaccesible", "servidor inacessível"]),
    ("network_error", ["erreur réseau : {}", "network error: {}", "error de red: {}", "erro de rede: {}"]),
    ("no_news", ["pas de nouveautés (HTTP {})", "no news (HTTP {})", "sin novedades (HTTP {})", "sem novidades (HTTP {})"]),
    ("unexpected_site", ["réponse inattendue du site", "unexpected answer from the website", "respuesta inesperada del sitio", "resposta inesperada do site"]),
    // PC checks and setup
    ("wine_system", ["Wine (système)", "Wine (system)", "Wine (sistema)", "Wine (sistema)"]),
    ("no_resolve", ["{} ne se résout pas", "{} does not resolve", "{} no se resuelve", "{} não é resolvido"]),
    ("hosts_wrong", ["{} → {} (attendu {})", "{} → {} (expected {})", "{} → {} (se esperaba {})", "{} → {} (esperado {})"]),
    ("hosts_missing", ["{} n'est pas dans le fichier hosts", "{} is not in the hosts file", "{} no está en el archivo hosts", "{} não está no arquivo hosts"]),
    ("cert_store_unreadable", ["magasin de certificats illisible", "certificate store unreadable", "almacén de certificados ilegible", "repositório de certificados ilegível"]),
    ("cert_ok_windows", ["certificat du serveur reconnu par Windows", "server certificate trusted by Windows", "certificado del servidor reconocido por Windows", "certificado do servidor reconhecido pelo Windows"]),
    ("cert_missing", ["certificat du serveur non installé", "server certificate not installed", "certificado del servidor no instalado", "certificado do servidor não instalado"]),
    ("cert_ok_macos", ["certificat du serveur approuvé par macOS", "server certificate trusted by macOS", "certificado del servidor aprobado por macOS", "certificado do servidor aprovado pelo macOS"]),
    ("cert_not_trusted", ["certificat du serveur non approuvé", "server certificate not trusted", "certificado del servidor no aprobado", "certificado do servidor não aprovado"]),
    ("cert_check_failed", ["impossible de lire les réglages de confiance de macOS", "could not read macOS trust settings", "no se pudo leer la configuración de confianza de macOS", "não foi possível ler as configurações de confiança do macOS"]),
    ("cert_ok_prefix", ["certificat installé dans le préfixe Wine", "certificate installed in the Wine prefix", "certificado instalado en el prefijo de Wine", "certificado instalado no prefixo do Wine"]),
    ("prefix_missing", ["préfixe Wine pas encore créé", "Wine prefix not created yet", "prefijo de Wine aún no creado", "prefixo do Wine ainda não criado"]),
    ("cert_not_in_prefix", ["certificat absent du préfixe Wine", "certificate missing from the Wine prefix", "certificado ausente del prefijo de Wine", "certificado ausente do prefixo do Wine"]),
    ("bad_portal", ["portail invalide", "invalid portal", "portal no válido", "portal inválido"]),
    ("admin_refused", ["autorisation administrateur refusée", "administrator permission refused", "permiso de administrador denegado", "permissão de administrador recusada"]),
    ("install_wait", ["attente de l'installation impossible", "could not wait for the installation", "no se pudo esperar a la instalación", "não foi possível aguardar a instalação"]),
    ("install_failed", ["échec de l'installation (code {})", "installation failed (code {})", "la instalación falló (código {})", "a instalação falhou (código {})"]),
    ("hosts_prompt", ["Forever Launcher : ajout du serveur dans le fichier hosts.", "Forever Launcher: adding the server to the hosts file.", "Forever Launcher: añadiendo el servidor al archivo hosts.", "Forever Launcher: adicionando o servidor ao arquivo hosts."]),
    ("hosts_refused", ["autorisation refusée pour le fichier hosts", "permission refused for the hosts file", "permiso denegado para el archivo hosts", "permissão recusada para o arquivo hosts"]),
    ("hosts_write_failed", ["écriture du fichier hosts impossible : {}", "could not write the hosts file: {}", "no se pudo escribir el archivo hosts: {}", "não foi possível gravar o arquivo hosts: {}"]),
    ("hosts_write_failed_pkexec", ["écriture du fichier hosts impossible (pkexec)", "could not write the hosts file (pkexec)", "no se pudo escribir el archivo hosts (pkexec)", "não foi possível gravar o arquivo hosts (pkexec)"]),
    ("cert_trust_failed", ["certificat non approuvé : {}", "certificate not trusted: {}", "certificado no aprobado: {}", "certificado não aprovado: {}"]),
    ("prefix_error", ["préfixe {} : {}", "prefix {}: {}", "prefijo {}: {}", "prefixo {}: {}"]),
    ("wine_not_found", ["wine est introuvable", "wine not found", "no se encontró wine", "wine não encontrado"]),
    ("umu_not_found", ["umu-run est introuvable (paquet umu-launcher)", "umu-run not found (umu-launcher package)", "no se encontró umu-run (paquete umu-launcher)", "umu-run não encontrado (pacote umu-launcher)"]),
    ("proton_not_found", ["Proton introuvable : {}", "Proton not found: {}", "Proton no encontrado: {}", "Proton não encontrado: {}"]),
    ("command_failed", ["{} a échoué : {}", "{} failed: {}", "{} falló: {}", "{} falhou: {}"]),
    // key patcher (journal)
    ("bad_public_key", ["clé publique du serveur invalide", "invalid server public key", "clave pública del servidor no válida", "chave pública do servidor inválida"]),
    ("bad_region_group", ["groupe de région invalide (2-12, sauf 9)", "invalid region group (2-12, except 9)", "grupo de región no válido (2-12, excepto 9)", "grupo de região inválido (2-12, exceto 9)"]),
    ("write_refused", ["écriture refusée", "write refused", "escritura rechazada", "gravação recusada"]),
    ("verify_failed", ["vérification échouée", "check failed", "verificación fallida", "verificação falhou"]),
    ("memory_changed", ["la mémoire a changé pendant l'écriture", "memory changed while writing", "la memoria cambió durante la escritura", "a memória mudou durante a gravação"]),
    ("key_restored", ["clé du serveur remise en place", "server key put back", "clave del servidor restablecida", "chave do servidor restaurada"]),
    ("key_write_error", ["écriture de la clé : {}", "writing the key: {}", "escribiendo la clave: {}", "gravando a chave: {}"]),
    ("key_store_freed", ["magasin de clés libéré, nouvelle recherche à la prochaine connexion", "key store freed, searching again at the next login", "almacén de claves liberado, nueva búsqueda en el próximo inicio de sesión", "repositório de chaves liberado, nova busca no próximo login"]),
    ("game_process", ["processus du jeu : PID {}", "game process: PID {}", "proceso del juego: PID {}", "processo do jogo: PID {}"]),
    ("key_written", ["clé du serveur écrite (groupe {})", "server key written (group {})", "clave del servidor escrita (grupo {})", "chave do servidor gravada (grupo {})"]),
    ("key_write_retry", ["écriture de la clé : {}, nouvel essai", "writing the key: {}, retrying", "escribiendo la clave: {}, reintentando", "gravando a chave: {}, tentando de novo"]),
    ("waiting_realm", ["en attente de l'entrée dans le royaume…", "waiting for the realm login…", "esperando la entrada al reino…", "aguardando a entrada no reino…"]),
    ("many_key_stores", ["{} magasins de clés à la fois : attente", "{} key stores at once: waiting", "{} almacenes de claves a la vez: esperando", "{} repositórios de chaves ao mesmo tempo: aguardando"]),
];

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn every_message_has_four_texts_with_the_same_placeholders() {
        for (key, texts) in MESSAGES {
            let n = texts[0].matches("{}").count();
            for text in texts {
                assert!(!text.is_empty(), "{key}");
                assert_eq!(text.matches("{}").count(), n, "{key}: {text}");
            }
        }
    }

    #[test]
    fn formats_in_the_chosen_language() {
        set("es-419");
        assert_eq!(tf("server_error", &[&503]), "error del servidor (HTTP 503)");
        set("pt-BR");
        assert_eq!(t("banned"), "conta banida");
        set("de");
        assert_eq!(t("banned"), "account banned");
        set("fr");
    }
}
