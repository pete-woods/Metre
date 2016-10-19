/***

Copyright 2013-2016 Dave Cridland
Copyright 2014-2016 Surevine Ltd

Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
of the Software, and to permit persons to whom the Software is furnished to do
so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

***/

#include <http.h>
#include "rapidxml.hpp"
#include "xmlstream.h"
#include "xmppexcept.h"
#include "netsession.h"
#include "feature.h"
#include "filter.h"
#include "router.h"
#include "config.h"
#include "log.h"
#include "tls.h"

#ifdef VALGRIND
#include <valgrind/memcheck.h>
#else
#define VALGRIND_MAKE_MEM_DEFINED_IF_ADDRESSABLE(ptr, len) 0
#endif

using namespace Metre;

XMLStream::XMLStream(NetSession *n, SESSION_DIRECTION dir, SESSION_TYPE t)
        : has_slots(), m_session(n), m_dir(dir), m_type(t) {
    if (t == X2X) {
        m_type = S2S;
        m_x2x_mode = true;
    }
}

XMLStream::XMLStream(NetSession *n, SESSION_DIRECTION dir, SESSION_TYPE t, std::string const &stream_local,
                     std::string const &stream_remote)
        : has_slots(), m_session(n), m_dir(dir), m_type(t), m_stream_local(stream_local),
          m_stream_remote(stream_remote) {
    if (t == X2X) {
        m_type = S2S;
        m_x2x_mode = true;
    }
}

void XMLStream::thaw() {
    if (!m_frozen) return;
    m_frozen = false;
    METRE_LOG(Log::DEBUG, "NS" << m_session->serial() << " - thaw");
    m_session->read();
    METRE_LOG(Log::DEBUG, "NS" << m_session->serial() << " - thaw done.");
}

size_t XMLStream::process(unsigned char *p, size_t len) {
    using namespace rapidxml;
    if (len == 0) return 0;
    if (m_frozen) {
        METRE_LOG(Log::DEBUG, "NS" << m_session->serial() << " - Data arrived when frozen");
        return 0;
    }
    (void) VALGRIND_MAKE_MEM_DEFINED_IF_ADDRESSABLE(p, len);
    size_t spaces = 0;
    for (unsigned char *sp{p}; len != 0; ++sp, --len, ++spaces) {
        switch (*sp) {
            default:
                break;
            case ' ':
            case '\r':
            case '\n':
                continue;
        }
        break;
    }
    if (spaces) m_session->used(spaces);
    if (len == 0) return spaces;
    std::string buf{reinterpret_cast<char *>(p + spaces), len};
    METRE_LOG(Metre::Log::DEBUG, "NS" << m_session->serial() << " - Got [" << len << "] : " << buf);
    try {
        try {
            if (m_stream_buf.empty()) {
                /**
                 * We need to grab the stream open. Do so by parsing the main buffer to find where the open
                 * finishes, and copy that segment over to another buffer. Then reparse, this time properly.
                 */
                METRE_LOG(Log::DEBUG, "Parsing stream open");
                char *end = m_stream.parse<parse_open_only | parse_fastest>(const_cast<char *>(buf.c_str()));
                auto test = m_stream.first_node();
                if (test && test->name()) {
                    m_stream_buf.assign(buf.data(), end - buf.data());
                    m_session->used(end - buf.data());
                    buf.erase(0, end - buf.data());
                    m_stream.parse<parse_open_only>(const_cast<char *>(m_stream_buf.c_str()));
                    stream_open();
                } else {
                    m_stream_buf.clear();
                }
            }
            while (!buf.empty()) {
                char *end = m_stanza.parse<parse_fastest | parse_parse_one>(const_cast<char *>(buf.c_str()), m_stream);
                auto element = m_stanza.first_node();
                if (!element || !element->name()) return len - buf.length();
                //std::cout << "TLE {" << element->xmlns() << "}" << element->name() << std::endl;
                m_session->used(end - buf.data());
                handle(element);
                buf.erase(0, end - buf.data());
                m_stanza.clear();
                if (m_frozen) return spaces + len - buf.length();
            }
        } catch (Metre::base::xmpp_exception) {
            throw;
        } catch (rapidxml::eof_error &e) {
            return spaces + len - buf.length();
        } catch (rapidxml::parse_error &e) {
            if (buf == "</stream:stream>") {
                m_session->send("</stream:stream>");
                m_closed = true;
                m_session->used(buf.size());
                buf.clear();
            } else {
                throw Metre::not_well_formed(e.what()); // TODO Also, need to freeze processing during pre-TLS lookups.
            }
        } catch (std::runtime_error &e) {
            throw Metre::undefined_condition(e.what());
        }
    } catch (Metre::base::xmpp_exception &e) {
        handle_exception(e);
    }
    return spaces + len - buf.length();
}

void XMLStream::handle_exception(Metre::base::xmpp_exception &e) {
    using namespace rapidxml;
    METRE_LOG(Metre::Log::INFO, "Raising error: " << e.what());
    xml_document<> d;
    auto error = d.allocate_node(node_element, "stream:error");
    auto specific = d.allocate_node(node_element, e.element_name());
    specific->append_attribute(d.allocate_attribute("xmlns", "urn:ietf:params:xml:ns:xmpp-streams"));
    auto text = d.allocate_node(node_element, "text", e.what());
    specific->append_node(text);
    if (dynamic_cast<Metre::undefined_condition *>(&e)) {
        auto other = d.allocate_node(node_element, "unhandled-exception");
        other->append_attribute(d.allocate_attribute("xmlns", "http://cridland.im/xmlns/metre"));
        specific->append_node(other);
    }
    error->append_node(specific);
    if (m_opened) {
        d.append_node(error);
        m_session->send(d);
        m_session->send("</stream:stream>");
    } else {
        auto node = d.allocate_node(node_element, "stream:stream");
        node->append_attribute(d.allocate_attribute("xmlns:stream", "http://etherx.jabber.org/streams"));
        node->append_attribute(d.allocate_attribute("version", "1.0"));
        node->append_attribute(d.allocate_attribute("xmlns", content_namespace()));
        node->append_node(error);
        d.append_node(node);
        m_session->send("<?xml version='1.0'?>");
        m_session->send(d);
    }
    m_closed = true;
}

void XMLStream::in_context(std::function<void()> &&fn, Stanza &s) {
    try {
        try {
        } catch (Metre::base::xmpp_exception) {
            throw;
        } catch (Metre::base::stanza_exception) {
            throw;
        } catch (std::runtime_error &e) {
            throw Metre::stanza_undefined_condition(e.what());
        }
    } catch (Metre::base::stanza_exception const &stanza_error) {
        std::unique_ptr<Stanza> st = s.create_bounce(stanza_error);
        std::shared_ptr<Route> route = RouteTable::routeTable(st->from()).route(st->to());
        route->transmit(std::move(st));
    } catch (Metre::base::xmpp_exception &e) {
        handle_exception(e);
    }
}

void XMLStream::in_context(std::function<void()> &&fn) {
    try {
        try {
        } catch (Metre::base::xmpp_exception &) {
            throw;
        } catch (Metre::base::stanza_exception &e) {
            throw Metre::undefined_condition(std::string("Uncaught stanza error: ") + e.what());
        } catch (std::runtime_error &e) {
            throw Metre::stanza_undefined_condition(e.what());
        }
    } catch (Metre::base::xmpp_exception &e) {
        handle_exception(e);
    }
}

const char *XMLStream::content_namespace() const {
    const char *p;
    switch (m_type) {
        case C2S:
            p = "jabber:client";
            break;
        case COMP:
            p = "jabber:component:accept";
            break;
        default:
        case S2S:
            p = "jabber:server";
            break;
    }
    return p;
}

void XMLStream::check_domain_pair(std::string const &from, std::string const &domainname) const {
    Config::Domain const &domain = Config::config().domain(domainname);
    if (domain.block()) {
        throw Metre::host_unknown("Requested domain is blocked.");
    }
    if (m_type == COMP && domain.transport_type() != COMP) {
        throw Metre::host_unknown("That would be me.");
    }
    Config::Domain const &from_domain = Config::config().domain(from);
    if (!from.empty()) {
        if (from_domain.block()) {
            throw Metre::host_unknown("Requesting domain is blocked");
        }
        if ((domain.transport_type() != COMP) &&
            ((from_domain.forward() == domain.forward()) && domain.transport_type() != INT &&
             from_domain.transport_type() != INT)) {
            throw Metre::host_unknown("Will not forward between those domains");
        }
        if (from_domain.transport_type() == COMP) {
            if (m_type != COMP) {
                throw Metre::host_unknown("Seems unlikely.");
            }
        }
    }
}

void XMLStream::stream_open() {
    /**
     * We may be able to change our minds on what stream type this is, here,
     * by looking at the default namespace.
     */
    auto stream = m_stream.first_node();
    auto xmlns = stream->first_attribute("xmlns");
    if (xmlns && xmlns->value()) {
        std::string default_xmlns(xmlns->value(), xmlns->value_size());
        if (default_xmlns == "jabber:client") {
            METRE_LOG(Metre::Log::DEBUG, "C2S stream detected.");
            m_type = C2S;
        } else if (default_xmlns == "jabber:server") {
            METRE_LOG(Metre::Log::DEBUG, "S2S stream detected.");
            m_type = S2S;
        } else if (default_xmlns == "jabber:component:accept") {
            METRE_LOG(Metre::Log::DEBUG, "114 stream detected.");
            m_type = COMP;
        } else {
            METRE_LOG(Metre::Log::WARNING, "Unidentified connection.");
        }
    }
    auto domainat = stream->first_attribute("to");
    std::string domainname;
    if (domainat && domainat->value()) {
        domainname.assign(domainat->value(), domainat->value_size());
        METRE_LOG(Metre::Log::DEBUG, "Requested contact domain {" << domainname << "}");
    } else if (m_dir == OUTBOUND) {
        domainname = Jid(m_stream_local).domain();
    } else {
        domainname = Config::config().default_domain();
    }
    std::string from;
    if (auto fromat = stream->first_attribute("from")) {
        from = Jid(std::string(fromat->value(), fromat->value_size())).domain();
    }
    METRE_LOG(Metre::Log::DEBUG, "Requesting domain is " << from);
    check_domain_pair(from, domainname);
    if (!stream->xmlns()) {
        throw Metre::bad_format("Missing namespace for stream");
    }
    if (stream->name() != std::string("stream") ||
        stream->xmlns() != std::string("http://etherx.jabber.org/streams")) {
        throw Metre::bad_namespace_prefix("Need a stream open");
    }
    // Assume we're good here.
    auto version = stream->first_attribute("version");
    std::string ver = "1.0";
    bool with_ver = false;
    if (version &&
        version->value() &&
        version->value_size() == 3 &&
        ver.compare(0, 3, version->value(), version->value_size()) == 0) {
        with_ver = true;
    }
    if (m_dir == INBOUND) {
        m_stream_local = domainname;
        m_stream_remote = from;
        if (m_stream_remote == m_stream_local) {
            throw std::runtime_error("That's me, you fool");
        }
        auto &r = RouteTable::routeTable(m_stream_local).route(m_stream_remote);
        r->onNamesCollated.connect(this, [this, with_ver](Route &) {
            send_stream_open(with_ver);
            if (with_ver) {
                rapidxml::xml_document<> doc;
                auto features = doc.allocate_node(rapidxml::node_element, "stream:features");
                doc.append_node(features);
                for (auto f : Feature::features(m_type)) {
                    f->offer(features, *this);
                }
                m_session->send(doc);
            }
            thaw();
        }, true);
        freeze();
        r->collateNames();
    } else if (m_dir == OUTBOUND) {
        if (m_type == S2S) {
            auto id_att = stream->first_attribute("id");
            if (id_att) {
                if (!m_stream_id.empty()) {
                    Router::unregister_stream_id(m_stream_id);
                }
                m_stream_id = id_att->value();
                Router::register_stream_id(m_stream_id, *m_session);
            }
        }
        return;
    }
}

void XMLStream::send_stream_open(bool with_version) {
    if (m_x2x_mode) {
        if (m_secured) {
            auto route = RouteTable::routeTable(m_stream_local).route(m_stream_remote);
            if (!tls_auth_ok(*route)) {
                throw host_unknown("Cannot authenticate host");
            }
        }
        m_stream_buf == "<stream:stream xmlns:stream='http://etherx.jabber.org/streams' xmlns='";
        m_stream_buf += content_namespace();
        m_stream_buf += "' to='";
        m_stream_buf += m_stream_local;
        m_stream_buf += "' from='";
        m_stream_buf += m_stream_remote;
        m_stream_buf += "' version='1.0'>";
    } else {
        /*
        *   We write this out as a string, to avoid trying to make rapidxml
        * write out only the open tag.
        */
        std::string open = "<stream:stream xmlns:stream='http://etherx.jabber.org/streams' xmlns='";
        open += content_namespace();
        if (m_type == S2S) {
            open += "' xmlns:db='jabber:server:dialback";
            if (m_stream_remote != "") {
                open += "' to='";
                open += m_stream_remote;
            }
        }
        open += "' from='";
        open += m_stream_local;
        if (m_dir == INBOUND) {
            open += "' id='";
            generate_stream_id();
            open += m_stream_id;
        }
        if (with_version) {
            open += "' version='1.0'>";
        } else {
            open += "'>";
        }
        m_session->send(open);
    }
    m_opened = true;
}

void XMLStream::send(rapidxml::xml_document<> &d) {
    m_session->send(d);
}

void XMLStream::send(std::unique_ptr<Stanza> s) {
    rapidxml::xml_document<> d;
    s->render(d);
    m_session->send(d);
}

void XMLStream::handle(rapidxml::xml_node<> *element) {
    std::string xmlns(element->xmlns(), element->xmlns_size());
    if (xmlns == "http://etherx.jabber.org/streams") {
        std::string elname(element->name(), element->name_size());
        if (elname == "features") {
            METRE_LOG(Metre::Log::DEBUG, "It's features!");
            for (;;) {
                rapidxml::xml_node<> *feature_offer = nullptr;
                Feature::Type feature_type = Feature::Type::FEAT_NONE;
                std::string feature_xmlns;
                for (auto feat_ad = element->first_node(); feat_ad; feat_ad = feat_ad->next_sibling()) {
                    std::string offer_name(feat_ad->name(), feat_ad->name_size());
                    std::string offer_ns(feat_ad->xmlns(), feat_ad->xmlns_size());
                    METRE_LOG(Metre::Log::DEBUG, "Got feature offer: {" << offer_ns << "}" << offer_name);
                    if (m_features.find(offer_ns) != m_features.end()) continue; // Already negotiated.
                    Feature::Type offer_type = Feature::type(offer_ns, *this);
                    METRE_LOG(Metre::Log::DEBUG, "Offer type seems to be " << offer_type);
                    switch (offer_type) {
                        case Feature::Type::FEAT_NONE:
                            continue;
                        case Feature::Type::FEAT_SECURE:
                            if (m_secured) continue;
                            break;
                        case Feature::Type::FEAT_COMP:
                            if (m_compressed) continue;
                            break;
                        default:
                            /* pass */;
                    }
                    if (feature_type < offer_type) {
                        METRE_LOG(Metre::Log::DEBUG,
                                  "I'll keep {" << offer_ns << "} instead of {" << feature_xmlns << "}");
                        feature_offer = feat_ad;
                        feature_xmlns = offer_ns;
                        feature_type = offer_type;
                    }
                }
                METRE_LOG(Metre::Log::DEBUG, "Processing feature {" << feature_xmlns << "}");
                if (feature_type == Feature::Type::FEAT_NONE) {
                    if (m_features.find("urn:xmpp:features:dialback") == m_features.end()) {
                        auto so = m_stream.first_node();
                        auto dbatt = so->first_attribute("xmlns:db");
                        if (dbatt && dbatt->value() == std::string("jabber:server:dialback")) {
                            feature_xmlns = "urn:xmpp:features:dialback";
                            goto try_feature;
                        }
                    } else if (s2s_auth_pair(local_domain(), remote_domain(), OUTBOUND) == AUTHORIZED) {
                        set_auth_ready();
                        onAuthenticated.emit(*this);
                    }
                    return;
                }
                try_feature:
                Feature *f = Feature::feature(feature_xmlns, *this);
                assert(f);
                try {
                    bool escape = f->negotiate(feature_offer);
                    m_features[feature_xmlns] = f;
                    METRE_LOG(Metre::Log::DEBUG, "Feature negotiated, stream restart is " << escape);
                    if (escape) return; // We've done a stream restart or something.
                } catch (...) {
                    delete f;
                }
            }
        } else if (elname == "error") {
            METRE_LOG(Metre::Log::DEBUG, "It's a stream error!");
            throw std::runtime_error("Actually, I have a bag of shite for error handling.");
        } else {
            METRE_LOG(Metre::Log::DEBUG, "It's something weird.");
            throw Metre::unsupported_stanza_type("Unknown stream element");
        }
    } else {
        auto fit = m_features.find(xmlns);
        Feature *f = 0;
        METRE_LOG(Metre::Log::DEBUG, "Hunting handling feature for {" << xmlns << "}");
        if (fit != m_features.end()) {
            f = (*fit).second;
        } else {
            f = Feature::feature(xmlns, *this);
            if (f) m_features[xmlns] = f;
            METRE_LOG(Metre::Log::DEBUG, "Created new feature << " << f);
        }

        bool handled = false;
        if (f) {
            handled = f->handle(element);
        }
        METRE_LOG(Metre::Log::DEBUG, "Handled: " << handled);
        if (!handled) {
            throw Metre::unsupported_stanza_type();
        }
    }
}

Feature &XMLStream::feature(const std::string &ns) {
    auto i = m_features.find(ns);
    if (i == m_features.end()) {
        throw std::runtime_error("Expected feature " + ns + " not found");
    }
    return *(i->second);
}

void XMLStream::restart() {
    // Check if I need to fetch CRLs at this point.
    if (!m_crl_complete && prep_crl(*this)) {
        return;
    }
    do_restart();
}

void XMLStream::do_restart() {
    if (!m_stream_id.empty()) {
        Router::unregister_stream_id(m_stream_id);
        m_stream_id.clear();
    }
    for (auto f : m_features) {
        delete f.second;
    }
    m_features.clear();
    m_stream.clear();
    m_stanza.clear();
    m_stream_buf.clear();
    if (m_dir == OUTBOUND) {
        if (m_secured) {
            auto &r = RouteTable::routeTable(m_stream_local).route(m_stream_remote);
            r->onNamesCollated.connect(this, [this](Route &) {
                send_stream_open(true);
            }, true);
            r->collateNames();
        } else {
            send_stream_open(true);
        }
    }
    thaw();
}

XMLStream::~XMLStream() {
    for (auto f : m_features) {
        delete f.second;
    }
}

void XMLStream::generate_stream_id() {
    if (!m_stream_id.empty()) {
        Router::unregister_stream_id(m_stream_id);
    }
    m_stream_id = Config::config().random_identifier();
    Router::register_stream_id(m_stream_id, *m_session);
}

XMLStream::AUTH_STATE
XMLStream::s2s_auth_pair(std::string const &local, std::string const &remote, SESSION_DIRECTION dir) const {
    if (m_type == COMP) {
        if (m_user) {
            if (dir == OUTBOUND && *m_user == remote) {
                return AUTHORIZED;
            } else if (dir == INBOUND && *m_user == remote) {
                return AUTHORIZED;
            }
        }
    }
    auto &m = (dir == INBOUND ? m_auth_pairs_rx : m_auth_pairs_tx);
    auto it = m.find(std::make_pair(local, remote));
    if (it != m.end()) {
        return (*it).second;
    }
    return NONE;
}

XMLStream::AUTH_STATE
XMLStream::s2s_auth_pair(std::string const &local, std::string const &remote, SESSION_DIRECTION dir,
                         XMLStream::AUTH_STATE state) {
    if (state == AUTHORIZED && !m_secured &&
        (Config::config().domain(local).require_tls() || Config::config().domain(remote).require_tls())) {
        throw Metre::not_authorized("Authorization attempt without TLS");
    }
    auto &m = (dir == INBOUND ? m_auth_pairs_rx : m_auth_pairs_tx);
    auto key = std::make_pair(local, remote);
    AUTH_STATE current = m[key];
    if (current < state) {
        m[key] = state;
        if (state == XMLStream::AUTHORIZED) METRE_LOG(Metre::Log::INFO,
                                                      "NS" << m_session->serial() << " - Authorized "
                                                           << (dir == INBOUND ? "INBOUND" : "OUTBOUND")
                                                           << " session local:" << local << " remote:"
                                                           << remote);
        onAuthenticated.emit(*this);
    }
    return m[key];
}

bool XMLStream::tls_auth_ok(Route &route) {
    if (!m_secured) return false;
    return verify_tls(*this, route);
}

void XMLStream::fetch_crl(std::string const &uri) {
    m_num_crls++;
    Http::crl(uri).connect(this, &XMLStream::add_crl);
    freeze();
}

void XMLStream::add_crl(std::string const &uri, int code, struct X509_crl_st *data) {
    if (m_crl_complete) return;
    if (code == 200) {
        METRE_LOG(Log::INFO, "NS" << m_session->serial() << " - Got CRL from URI " << uri);
        m_crls[uri] = data;
        if (m_crls.size() == m_num_crls) {
            m_crl_complete = true;
            do_restart();
        }
    }
}

void XMLStream::crl(std::function<void(struct X509_crl_st *)> const &fn) {
    for (auto pair : m_crls) {
        fn(pair.second);
    }
}